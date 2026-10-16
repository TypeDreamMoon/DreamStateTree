# Extending DreamStateTree

For authors of a game or of another plugin. DreamStateTree knows no game's nodes and no game's schemas. It learns them through
four layers, from nothing to write to a C++ class:

| Layer | Who writes it | What it gives |
|---|---|---|
| L0 reflection | nobody | any StateTree node struct, Blueprint node class, schema, enum, struct and tag works as it is |
| L1 metadata | `meta = (...)` on your structs, classes and properties | names and namespaces, old names, positional arguments, a schema's default namespaces, validation tags, the contexts a schema takes |
| L2 libraries | `.dth` files | expression templates, shared subtrees, tag prefixes |
| L3 extension | a `UDreamStateTreeExtension` subclass in an editor module | extra checks, default event payloads per tag |

Use the lowest layer that does the job. The names in this page are those of an imaginary game with villagers (`Villager`,
`Walk`, `GoTo`, `Hour`, `Chance`); nothing in DreamStateTree depends on them.

- [L0: reflection](#l0-reflection)
- [L1: metadata](#l1-metadata)
- [L2: libraries](#l2-libraries)
- [L3: extensions](#l3-extensions)
- [Rules that must hold for every tree](#rules-that-must-hold-for-every-tree)
- [Where sources live](#where-sources-live)

## L0: reflection

Nothing to write. A node is any `USTRUCT` derived from the engine's task, condition, evaluator, consideration or property
function base (`FStateTreeTaskBase`, `FStateTreeConditionBase`, `FStateTreeEvaluatorBase`, `FStateTreeConsiderationBase`,
`FStateTreePropertyFunctionBase`), or a Blueprint class derived from the Blueprint base of a task, condition, evaluator or
consideration. A schema is any `UStateTreeSchema` subclass that is not abstract. DreamStateTree finds them by reflection and by
the asset registry, and a schema's own `IsStructAllowed`, `IsClassAllowed` and `Allow...` functions decide what a tree of that
schema may hold.

Without any metadata:

- A node struct is written by its name without the `F`, without a leading `StateTree`, and without a trailing `Task`,
  `Condition`, `Evaluator`, `Consideration` or `PropertyFunction`: `FVillagerWalkTask` is `VillagerWalk`. Its struct name
  without the `F` (`VillagerWalkTask`) works too. A name that two nodes share is DST4002 until one of them is given another
  name (L1).
- A Blueprint node is written by its asset's name, or with `Path(Game, "AI/STT_Wander")`.
- A schema is written by its class name without `U`, `StateTree` and `Schema`: `UVillagerStateTreeSchema` is `Villager`.
- The arguments of a call are the properties of the node's instance data, named as in C++ (`Speed`, `bRun` or `Run`), then the
  editable properties of the node struct. A property's category decides how it is used: `Category = "Input"` must be bound,
  `"Parameter"` takes a constant or a binding, `"Output"` is read by others, `"Context"` is filled in by the compiler.
  `meta = (Optional)` lets an Input stay unbound. A property reference is a `FStateTreePropertyRef` and is written with `ref`.
- A struct marked `meta = (Hidden)` is not a node (it is how the engine marks the bases games derive from).
- Constants of the property types work: numbers, strings, enums, structs as tuples, arrays, gameplay tags, assets by name,
  classes, references to other StateTrees ([Language.md](Language.md#constants)). Bindings may feed a member of a struct property, an
  element of an array property, or a parameter of a tree reference ([Language.md](Language.md#parts-fed-from-elsewhere)).

## L1: metadata

All keys are plain `meta` strings. A runtime module can carry them without depending on DreamStateTree.

| Key | Put on | Effect |
|---|---|---|
| `DreamStateTree = "Villager.Walk"` | a node `USTRUCT`, a Blueprint node base `UCLASS`, a schema `UCLASS` | The name the text uses. It may include a namespace. It replaces the derived name; the reflected struct name still works. |
| `DreamStateTreeAliases = "Villager.Move;Move"` | the same | Old names that still resolve, separated by `;`. Rename a node and old texts keep building. The decompiler writes the current name. |
| `DreamStateTreeArgs = "Factor,Run"` | a node `USTRUCT` | The properties that may be passed by position, in order, separated by `,`. The names are the C++ property names (`bRun`, not `Run`). |
| `DreamStateTreeNamespace = "Villager;Game"` | a schema `UCLASS` | Default namespaces of the trees of that schema, separated by `;`, searched in order. A schema that derives from yours inherits them unless it sets its own. |
| `DreamStateTreeTags = "Instant;NeedsActor"` | a node `USTRUCT` | Free tags, separated by `;`, for your L3 `Validate` to read (see below). |
| `DreamStateTreeContext = "Actor"` | a `UPROPERTY` of a schema | The name of the context this property sets, for the header's `Schema=Name(Actor = ...)`. |

### A node

```cpp
USTRUCT(meta = (DisplayName = "Walk",
                DreamStateTree = "Villager.Walk",
                DreamStateTreeAliases = "Villager.Move;Move",
                DreamStateTreeArgs = "Factor,Run"))
struct FVillagerWalkTask : public FStateTreeTaskCommonBase
{
    GENERATED_BODY()

    using FInstanceDataType = FVillagerWalkInstanceData;
    // ...
};

USTRUCT()
struct FVillagerWalkInstanceData
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, Category = "Parameter")
    float Factor = 1.f;

    UPROPERTY(EditAnywhere, Category = "Parameter")
    bool bRun = false;

    UPROPERTY(EditAnywhere, Category = "Input")
    TObjectPtr<AActor> Target = nullptr;
};
```

Now `Walk(0.8, true)` means `Factor = 0.8, Run = true`, `Walk(Target = Player.Pawn)` binds the input, and `Move(...)` is still
accepted. In a tree of a schema with the namespace `Villager`, `Walk` is enough; elsewhere it is `Villager.Walk`.

Positions are looked up by property name in the instance data first, then in the node struct. A property named in
`DreamStateTreeArgs` that does not exist is DST4007.

### A schema and its contexts

```cpp
UCLASS(meta = (DreamStateTree = "Villager", DreamStateTreeNamespace = "Villager;Game"))
class UVillagerStateTreeSchema : public UStateTreeSchema
{
    GENERATED_BODY()

public:
    UPROPERTY(EditAnywhere, Category = "Defaults", meta = (DreamStateTreeContext = "Pawn"))
    TSubclassOf<AVillagerPawn> ContextPawnClass = AVillagerPawn::StaticClass();

    // ...
};
```

A tree of this schema is written `Schema=Villager` and gets the namespaces `Villager`, then `Game`. To set a context's class
the header says `Schema=Villager(Pawn = BP_Farmer)`; the key is the name in `DreamStateTreeContext`, or the name of the
property itself (`ContextPawnClass`).

#### Contexts

- A context is data the schema provides (`GetContextDataDescs()`). Paths start at the context's name: `Pawn.Health`.
- The engine's own component schemas need no metadata: `Actor` sets `ContextActorClass` and `AIController` sets
  `AIControllerClass`.
- After setting a schema property the build calls `PostEditChangeChainProperty` on the schema, as the Details panel does. If
  your schema derives its context list from the property there (the engine's component schema does), it is updated.
- A name that is not a context or a property of the schema is DST4019; a global node or a task cannot take a context's name
  (DST6015).

### Validation tags

`DreamStateTreeTags` does nothing by itself. Your extension reads it from the name registry:

```cpp
const UE::DreamStateTree::Compiler::FDstNodeType* Type =
    UE::DreamStateTree::Compiler::FDstNameRegistry::Get().FindByStruct(Node.Node.GetScriptStruct());
const bool bInstant = Type && Type->Tags.Contains(TEXT("Instant"));
```

## L2: libraries

A `.dth` library holds `Tags`, `Template` and `Subtree` ([Language.md](Language.md#libraries)). Put the library in the `DTree/`
folder of your project or plugin; files include it by name:

```dtr
StateTreeLibrary(Name="AI/VillagerCommon")
{
    Tags = { Game.Event.Villager; Game.Ask.Villager; }

    Template Nervous = Chance(1 - Params.Courage);
    Template Daytime(From, To) = Hour(From, To) and not Raining();

    Subtree HitReaction(float Toughness = 1) {
        if ObjectIsValid(Actor)
        State Stagger { on Hit.Stagger  do Stagger(0.8 / Params.Toughness) }
    }
}
```

```dtr
#include "VillagerCommon.dth"

StateTree(Name="AI/ST_Farmer", Schema=Villager)
{
    State Root {
        State Hurt  { link HitReaction(Toughness = 2) }
        State Night { if Daytime(22, 5)  do GoTo(Params.Home) }
    }
}
```

- A **template** is expanded where it is used, so it works with any node of any schema.
- A **subtree** is copied into every tree that links it. Use it for behavior that many trees share but that has to live inside
  each asset, as the engine requires.
- **Tags** declares prefixes for the files that include the library. The text never adds tags to the project; the tags must be
  in the project's tag table.
- A `.dth` builds nothing by itself; it is built through the trees that include it.

## L3: extensions

An extension adds checks that only text needs and answers questions the text cannot. It is a `UObject` subclass in an **editor**
module of your game or plugin; there is nothing to register: every class that derives from `UDreamStateTreeExtension` and is not
abstract is found, and its default object answers. The module depends on `DreamStateTreeCompiler`:

```cs
PrivateDependencyModuleNames.AddRange(new string[] { "DreamStateTreeCompiler", "StateTreeModule", "GameplayTags" });
```

Keep it in a module of its own, of type `Editor` (or `UncookedOnly`), so that the rest of the game does not need DreamStateTree.

```cpp
#include "DreamStateTreeExtension.h"

UCLASS()
class UVillagerStateTreeExtension : public UDreamStateTreeExtension
{
    GENERATED_BODY()

public:
    // Which trees this extension serves. Asked once per tree and build.
    virtual bool AppliesTo(const UStateTreeSchema& Schema) const override;

    // The payload type `on <Tag>` gets when the text names none; nullptr: no default.
    virtual const UScriptStruct* GetDefaultEventPayload(const UStateTreeSchema& Schema, const FGameplayTag& Tag) const override;

    // Checks once the tree is generated and compiled.
    virtual void Validate(const UStateTreeEditorData& EditorData, FDreamStateTreeDiagnostics& Out) const override;
};
```

| Function | When | What |
|---|---|---|
| `AppliesTo` | for every build and check of a tree | `true` when the extension serves trees of this schema. The default says `false`. Several extensions may apply to one tree; they are asked in the order of their class path. |
| `GetDefaultEventPayload` | while the text is checked, for each `on Tag` and each event trigger with no payload written | The struct that carries the event. A type written in the text wins. The first extension that gives one wins. |
| `Validate` | after the tree was generated and compiled without errors; during `build`, not during `check` | Report what is wrong, in `Out`. |

### Default payloads

```cpp
const UScriptStruct* UVillagerStateTreeExtension::GetDefaultEventPayload(const UStateTreeSchema& Schema, const FGameplayTag& Tag) const
{
    static const FGameplayTag Trade = FGameplayTag::RequestGameplayTag("Game.Ask.Villager.Trade");
    return Tag.MatchesTag(Trade) ? FVillagerTradeOffer::StaticStruct() : nullptr;
}
```

With that, `on Trade` (given the prefix `Game.Ask.Villager`) can read `Event.Offer`, without writing `on Trade(VillagerTradeOffer)`.

### Reporting

```cpp
void UVillagerStateTreeExtension::Validate(const UStateTreeEditorData& EditorData, FDreamStateTreeDiagnostics& Out) const
{
    EditorData.VisitHierarchy([&](UStateTreeState& State, UStateTreeState*)
    {
        if (State.Tasks.Num() > 4)
        {
            Out.Warning("VILL0001", State.ID, TEXT("A state with more than four tasks is hard to read."));
        }
        return EStateTreeVisitor::Continue;
    });
}
```

`FDreamStateTreeDiagnostics` has `Report(Severity, Code, Where, Message)`, `Error(Code, Where, Message)` and
`Warning(Code, Where, Message)`. `Where` is any ID in the tree's editor data:

| `Where` | Lands on |
|---|---|
| a state's `ID` | the state's name in its card |
| a node's ID (`FStateTreeEditorNode::ID`) | the node's call |
| a transition's `ID` | the transition's line |
| a parameter's property ID | the parameter's declaration |
| an invalid `FGuid` | the file header |

DreamStateTree finds the line from the ID because the build derives every ID from the thing's place in the text
([Mapping.md](Mapping.md#identity)). A report is shown as DST7007 with your code first in the message
(`VILL0001: A state with more than four tasks...`), at the severity you gave. Use your own prefix for codes; DreamStateTree only
places and shows them.

## Rules that must hold for every tree

A rule that should hold for every tree of a schema, built from text or by hand in the editor, does not belong in L3: text is not
the only way a tree is made. Put it in your **editor schema**, where a failing rule fails the engine's compile whoever made the
tree:

1. Derive `UStateTreeEditorSchema` and register it with `FStateTreeEditorModule::RegisterEditorSchemaClass(YourSchema, YourEditorSchema)`.
2. Override `HandlePostInternalCompile`. Write errors to the compile log and return `false`: the compile fails.

```cpp
bool UVillagerStateTreeEditorSchema::HandlePostInternalCompile(const UE::StateTree::Compiler::FPostInternalContext& Context)
{
    FStateTreeCompilerLog& Log = Context.GetLog();
    bool bOk = true;
    Context.GetEditorData()->VisitHierarchy([&](UStateTreeState& State, UStateTreeState*)
    {
        FStateTreeCompilerLogStateScope Scope(&State, Log);
        if (State.Type == EStateTreeStateType::Group && State.Tag.IsValid())
        {
            Log.Reportf(EMessageSeverity::Error, FStateTreeBindableStructDesc(), TEXT("A group has no tag in this game."));
            bOk = false;
        }
        return EStateTreeVisitor::Continue;
    });
    return bOk;
}
```

DreamStateTree places the compile log's messages in the source as it places the engine's own: a message that names an item (a
node, a parameter) by ID lands on that item's line, one that names only a state lands on the state, otherwise on the header. An
error is DST7003, a warning DST7004.

Other switches the engine gives a schema work for text as well:

- **Which nodes may be used**: `IsStructAllowed` and `IsClassAllowed`. DreamStateTree refuses a node the schema does not allow
  with DST4003, before generating anything.
- **Which features exist**: `AllowEvaluators`, `AllowEnterConditions`, `AllowUtilityConsiderations`, `AllowMultipleTasks`,
  `AllowGlobalParameters`, `AllowTasksCompletion`, `IsStateTypeAllowed`, `IsStateSelectionAllowed`. A tree that uses a feature
  the schema forbids is DST4015, again before generating anything. (The editor itself drops such things silently the next time
  it opens the tree, so the check matters.)

Two things to avoid:

- `FStateTreeEditorModule::OnValidateStateTree` is for fixing data up. It takes the asset, has no way to report an error, and
  runs on every edit. It is not a place for checks.
- A check that also lives in `Validate` should be one function that takes the editor data and a report callback and does not
  include DreamStateTree; the editor schema and the extension both call it, the extension only forwarding the reports. A project
  that does not have DreamStateTree installed keeps the rule.

## Where sources live

`.dtr` and `.dth` files are looked for in `DTree/` folders:

| Folder | Order |
|---|---|
| the project's `DTree/` | first |
| `DTree/` of every enabled plugin that has one | then, by plugin name |

The command line and the editor use the same roots. A project that depends on a plugin keeps its sources in its own `DTree/` or
in the plugin's; a plugin with its own `DTree/` (a library of shared subtrees, templates and trees) ships it in its plugin folder,
next to `Source/` and `Content/`. An `#include` is looked up next to the including file first, then in each root in that order,
so a plugin's library can be included by name from the project:

```dtr
#include "VillagerCommon.dth"
```

A source in a plugin names the asset's root with the header's `Root="Plugin.<Name>"`; leave it out for the project's content.

A packaged game needs none of this: the assets are ordinary StateTrees, and the sources are not cooked.
