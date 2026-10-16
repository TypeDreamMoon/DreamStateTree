# Text and engine

How a `.dtr` file maps onto the engine's StateTree editor data (`UStateTreeEditorData`, `UStateTreeState`,
`FStateTreeEditorNode`, `FStateTreeTransition`). The build applies these rules from text to asset; the decompiler applies them
backwards. The two are inverses: building a text, reading the asset back and printing it gives the same text again, up to the
text's own layout. The language is in [Language.md](Language.md), the diagnostics in [Diagnostics.md](Diagnostics.md).

Between text and asset sits a model (`Source/DreamStateTreeLang/Public/Model/DstModel.h`) that is shaped like the engine: node
types are struct paths, property names are the engine's, constants are the engine's export text, conditions are a flat list
with an operand and an indent, comparisons are Compare nodes and arithmetic is property function nodes. The conveniences of the
text (short names, schema namespaces, positional arguments, tag prefixes, comparisons, operators, templates) are applied on the
way in and recovered on the way out. The table of the conveniences is `Source/DreamStateTreeCompiler/Private/DstSugar.h`.

## The asset

| Engine | Text |
|---|---|
| `UStateTree` | the asset named by the header (`Name`, `Root`) |
| `UStateTreeEditorData` | made with the asset; an existing one is kept when it is of the right class, so an open editor survives a rebuild |
| `Schema` | the header's `Schema=` ([Schemas](Language.md#schemas)) |
| schema properties (context classes) | the arguments of `Schema=Name(...)` |
| `EditorSchema` | created from the schema (`FStateTreeEditorModule::GetEditorSchemaClass`) |
| `Extensions` | not written; the editor's view settings (`StateCentricViewEditorDataExtension`) are kept as they are on a rebuild; any other extension stops the decompiler (DST8002) |
| `RootParameterPropertyBag` | the `Parameters` section |
| `Evaluators`, `GlobalTasks` | the `Globals` section: evaluators first, then global tasks |
| `GlobalTasksCompletion` and the other editable properties of the editor data | the `Settings` section |
| `Colors` | the `Colors` section |
| `SubTrees` | the top-level `State` and `Subtree` members |
| `EditorBindings` | made from the paths, operations, `ref`, `->` and `Delegate(...)` in the text |
| breakpoints, `bExpanded`, `Parent` pointers | not written: editor state, or derived |

The package of a built asset carries two metadata values: `DreamStateTree.Source`, the text the asset was last built from or
written to, and `DreamStateTree.SourcePath`, its path below its source root. The first is the base the editor compares and
merges against ([WriteBack.md](WriteBack.md)).

## Parameters

| Text | Engine |
|---|---|
| `Type Name = Default;` in `Parameters` | a property of the root parameter property bag |
| `[ Category = "..." ]` on it | the bag property's metadata |
| `Subtree Name(Type Param = Default)` | the parameters of the subtree's state |
| `link Name(Param = value)` | the parameters of the linked state: the target's layout, with overrides on the ones written |

Types map to property bag types:

| Text | Bag type |
|---|---|
| `bool`, `int`/`int32`, `int64`, `float`, `double` | Bool, Int32, Int64, Float, Double |
| `Name`, `String`, `Text` | Name, String, Text |
| a struct, an enum | Struct, Enum (with the type) |
| a class name | Object (an instance of the class) |
| `Class<T>`, `SoftClass<T>`, `SoftObject<T>` | Class, SoftClass, SoftObject |
| `Array<T>`, `Set<T>` | the container flags of the bag property; at most two levels |

## States

| Engine (`UStateTreeState`) | Text |
|---|---|
| `Name` | the card's name |
| `Type` | State: a card. Group: `[ Group ]`. Linked: `link`. LinkedAsset: `link asset`. Subtree: a top-level `Subtree` |
| `SelectionBehavior` | `pick` ([below](#pick)) |
| `TasksCompletion` | `[ Complete = Any ]` or `All` |
| `Tag` | `[ Tag = ... ]` |
| `ColorRef` | `[ Color = Name ]`, resolved against the `Colors` section |
| `Description` | `[ Description = "..." ]` |
| `bEnabled` | `[ Enabled = false ]` |
| `Weight` | `[ Weight = n ]` |
| `CustomTickRate`, `bHasCustomTickRate` | `[ TickRate = n ]` (the toggle is switched on when the key is written) |
| `bCopyParameterBindingsOnTick`, `bCheckPrerequisitesWhenActivatingChildDirectly` | the settings of the same names |
| `Parameters.bFixedLayout` | `[ bFixedLayout ]`; set by itself for a linked state |
| `Parameters` | a subtree's signature; for a linked state, the overrides in the `link` |
| `LinkedSubtree`, `LinkedAsset` | `link`, `link asset` (set with `SetLinkedState` and `SetLinkedStateAsset`, as the editor does) |
| `bHasRequiredEventToEnter`, `RequiredEventToEnter` | `on Tag`, `on Tag(Payload)`, with `[ ConsumeEvent ]` for `bConsumeEventOnSelect` |
| `EnterConditions` | the `if` clauses, joined with `and` |
| `Considerations` | `utility` |
| `Tasks`, `SingleTask` | `do`; a schema with one task per state keeps its task in `SingleTask` |
| `Transitions` | the `->` clauses, in order |
| `Children` | the child cards |
| `ID` | derived ([Identity](#identity)) |

### pick

| Text | `EStateTreeStateSelectionBehavior` |
|---|---|
| `pick InOrder` (default) | `TrySelectChildrenInOrder` |
| `pick Random` | `TrySelectChildrenAtRandom` |
| `pick HighestUtility` | `TrySelectChildrenWithHighestUtility` |
| `pick WeightedUtility` | `TrySelectChildrenAtRandomWeightedByUtility` |
| `pick FollowTransitions` | `TryFollowTransitions` |
| `pick Enter` | `TryEnterState` |
| `pick None` | `None` |
| `pick UniformRandom` | `TrySelectChildrenAtRandom` (the engine's old name of the same value) |
| `pick RelativeUtility` | `TrySelectChildrenAtRandomWeightedByUtility` (likewise) |

A link sets the state's selection to `TryEnterState` itself.

## Transitions

| Engine (`FStateTreeTransition`) | Text |
|---|---|
| `State` (`FStateTreeStateLink`) and its `LinkType` | the target, below |
| `Trigger` | the trigger, below |
| `RequiredEvent` | `on Tag`, `on Tag(Payload)`, with `[ ConsumeEvent = false ]` for `bConsumeEventOnSelect` |
| `DelegateListener` | `on Delegate(path)` |
| `Conditions` | `if` |
| `Priority` | `[ Priority = ... ]` |
| `bDelayTransition`, `DelayDuration`, `DelayRandomVariance` | `[ Delay = ... ]`, `[ Variance = ... ]` (either switches the delay on) |
| `ReactivateTargetState` | `[ Reactivate ]` sets `ForceChanged`; `ForceSustained` has no text form (DST8004 warning) |
| `bTransitionEnabled` | `[ Enabled = false ]` |
| `ID` | derived |

| Text target | `EStateTreeTransitionType` |
|---|---|
| a state name or path | `GotoState`, to that state |
| `Next` | `NextState` |
| `NextSelectable` | `NextSelectableState` |
| `Parent` | `Parent` |
| `NextParent` | `NextParent` |
| `NextSelectableParent` | `NextSelectableParent` |
| `TreeSucceeded` | `Succeeded` |
| `TreeFailed` | `Failed` |

| Text trigger | `EStateTreeTransitionTrigger` |
|---|---|
| none, `Completed` | `OnStateCompleted` |
| `Succeeded` | `OnStateSucceeded` |
| `Failed` | `OnStateFailed` |
| `Tick` | `OnTick` |
| a tag | `OnEvent`, with `RequiredEvent` |
| `Delegate(...)` | `OnDelegate`, with `DelegateListener` bound to the dispatcher or to a task's completion |

A name or path target is matched against the end of the path of every state of the tree; it has to match one state
([Targets](Language.md#targets)). A transition listening to a task's `.Succeeds`, `.Fails` or `.Completes` binds its delegate
listener to the task's completion; the engine's compiler creates the dispatcher.

## Nodes

A node of the text is a call, `Name(args) [ settings ]`; in the engine it is an `FStateTreeEditorNode`.

| Engine | Text |
|---|---|
| the node's struct type | the call's name ([Node names](Language.md#node-names-and-namespaces)) |
| `Node` (the struct) and `Instance` (the instance data), or `InstanceObject` for a Blueprint node | the arguments |
| `Name` | the name of a task (`Name = Call()`); for a node with no place for a name before it, `[ Name = "..." ]` |
| `bTaskEnabled` | `[ Enabled = false ]` |
| `bConsideredForCompletion` | `[ ConsideredForCompletion = false ]` |
| `TransitionHandlingPriority` | `[ TransitionPriority = ... ]` |
| `EvaluationMode` | `[ Evaluation = ... ]` |
| `ExpressionIndent`, `ExpressionOperand` | `and`, `or`, `*` and parentheses ([below](#conditions-operands-and-indents)) |
| `bInvert` of a condition | `not` |
| `ExecutionRuntimeData` | not written: runtime data |
| `ID` | derived |

**Arguments** are properties. A named argument finds its property first in the instance data (the `Input`, `Parameter`,
`Output` and `Context` properties), then among the editable properties of the node struct itself. A node struct property that
is not one of the settings above reads as an argument when the instance data has no property of that name.
A Blueprint node's arguments are the variables of its Blueprint; the Blueprint wrapper struct's own properties are not arguments.

A model records only the properties that differ from the type's default (the struct's default, or a Blueprint node's class
default object) and the properties that are bound.

| Category of the property | In the text |
|---|---|
| Input | must be a binding |
| Parameter | a constant or a binding |
| Output | not written; `Prop -> path` copies it |
| Context | not written; the compiler binds it by type. An explicit binding is written when it differs from that |

### Bindings

The editor's property bindings (`EditorBindings`) are made from the paths in the text. Each binding goes from a source to a
target property of a node, a parameter or a transition.

| Source (the root of the path) | `EDstSourceKind` | Engine source |
|---|---|---|
| `Params.X` | Parameters | the root parameters struct of the tree |
| `Params.X` in a subtree | StateParameters | the parameters struct of the subtree's state |
| a context name | Context | the schema's context data (by its name and ID) |
| a global node | Global | the global task or evaluator's instance data |
| a named task or a property function | Node | the node's instance data, by the node's ID |
| `Event.X` | StateEvent | the required event of a state: its `Tag`, `Origin` or `Payload` |
| `Event.X` in a transition's `if` | TransitionEvent | the event of that transition |

| Text | Binding |
|---|---|
| `Prop = path` | the source copied into the property |
| `Prop = ref path` | a property reference to the source |
| `Prop -> Params.X` | the node's property copied into the parameter (the source of the binding is the destination) |
| `Prop = a + b`, `Prop = F(x)` | a property function node, created inside the binding, whose result is bound to the property |
| `Prop = (Min = path, Max = 5)` | a binding into the member: the target path is `Prop.Min` ([Parts of a property](#parts-of-a-property)) |
| `Prop = [(Value = path)]` | a binding into a member of an element: `Prop[0].Value` |
| `Prop = Tree(Param = path)` | a binding into a parameter of a reference to a tree: `Prop.Parameters.Value<PropertyBag>.Param`, the parameter marked as overridden |
| `link Name(Param = path)` | the target state's parameter fed from the path, with the parameter marked as overridden |
| `on Delegate(path)` | the transition's delegate listener |
| a task's `.Succeeds` / `.Fails` / `.Completes` | the task's completion dispatcher |

### Parts of a property

A binding may feed only a part of a property: a member of a struct, an element of an array, a member of an element
(`Waypoints[1].Offset.X`), or a parameter of a referenced tree. Its target path has a segment per step; a segment that picks an
array element carries the index. The property keeps its constant, and each bound part has an argument of its own:

| | In the model |
|---|---|
| the property | one constant argument: the export text of the whole property |
| each bound part | an argument with the longer path (`Range.Min`, `QueryConfig[0].Value`) |

The constant has the bound parts at the default a text gives them, so that the extractor and the lowering write the same text:
a field a tuple does not write is the struct's default (`FLinearColor` and `FColor` opaque), an element or a parameter bound as
a whole is the type's default, and a struct element is the struct's default. The extractor puts each bound part back to that
default before it exports the property, whatever the asset stored there (the binding overwrites it when the tree runs). The
constant is a model argument when it differs from the property's default after that. A linked state's parameter whose bindings feed
a part of it is treated the same.

Raise writes the constant as a tuple or list and puts the bound parts into it in the order the struct declares its fields.

### References to other trees

An `FStateTreeReference` property holds a tree asset, a property bag of values for that tree's parameters, and the IDs of the
parameters that are overridden (`PropertyOverrides`). The text writes `Tree(Param = value)`.

| Engine | Text |
|---|---|
| `StateTree` (the asset) | the callee: the asset's short name, or `Path(Root, "Rel")` |
| the bag's value of an overridden parameter | the argument `Param = value` |
| `PropertyOverrides` | which parameters are written as arguments, in the order of the tree's parameters |
| a parameter of the bag that is not overridden | not written: it follows the tree (`SetStateTree` copies the tree's defaults) |
| a binding into `StateTree.Parameters.Value.Param` | the argument `Param = path`; the parameter is overridden |

The constant is the reference's export text, which names the bag's struct (`/Engine/Transient.PropertyBag_<hash>`, made by the
engine from the tree's parameters, so the hash changes with their IDs). The extractor first brings the reference in step with its
tree (`SyncParameters`), puts the overrides in the order of the tree's parameters, marks the parameters that are bound as
overridden, and puts the bound parts at their defaults, so the same text gives the same constant. The model does not name the bag
in a path: the segment that enters it has the instance type `PropertyBag`, and the generator takes the struct the reference holds
there. A model with a reference can only be generated while the referenced tree is loaded.

Steps the generator takes that the editor takes when a binding is made (they are why a build is not just filling in fields):
every node a binding is made into is told with `OnBindingChanged` (a Compare Enum condition takes its enum from its binding);
links go through `SetLinkedState` and `SetLinkedStateAsset`; transitions through `AddTransition`; bindings are made last, when
every node and parameter exists; the build goes through the engine's compiler manager; and a compile that changes the editor
data is reported as a bug (DST7005).

### Comparisons

| Text, by the type of the side that reads something | Node | Operator |
|---|---|---|
| float, double | `StateTreeCompareFloatCondition` | `Operator` |
| int | `StateTreeCompareIntCondition` | `Operator` |
| bool | `StateTreeCompareBoolCondition` | `==`, `!=` through `bInvert` |
| enum | `StateTreeCompareEnumCondition` | `==`, `!=` through `bInvert` |
| `Name` | `StateTreeCompareNameCondition` | `==`, `!=` through `bInvert` |

| Spelling | Operator value |
|---|---|
| `<` | `Less` |
| `<=` | `LessOrEqual` |
| `==` | `Equal` |
| `!=` | `NotEqual` |
| `>=` | `GreaterOrEqual` |
| `>` | `Greater` |

The bound side is the node's `Left` (`bLeft`); the other side is `Right` (`bRight`), a constant or a binding. A bare bool path
`if Player.Sees` is a Compare Bool with `Right` true. An int against a float, or against a fraction, is a Compare Float.
`not C` sets `bInvert`.

### Operations

An operator in an argument is a property function node bound into the property. The type of the property being fed picks the
family.

| Text | Float / double | Int | Bool |
|---|---|---|---|
| `a + b` | `StateTreeAddFloatPropertyFunction` | `StateTreeAddIntPropertyFunction` | |
| `a - b` | `StateTreeSubtractFloatPropertyFunction` | `StateTreeSubtractIntPropertyFunction` | |
| `a * b` | `StateTreeMultiplyFloatPropertyFunction` | `StateTreeMultiplyIntPropertyFunction` | |
| `a / b` | `StateTreeDivideFloatPropertyFunction` | `StateTreeDivideIntPropertyFunction` | |
| `-a` | `StateTreeInvertFloatPropertyFunction` | `StateTreeInvertIntPropertyFunction` | |
| `a and b` | | | `StateTreeBooleanAndPropertyFunction` |
| `a or b` | | | `StateTreeBooleanOrPropertyFunction` |
| `not a` | | | `StateTreeBooleanNotPropertyFunction` |

The binary functions have `Left`, `Right` and `Result` (the Boolean ones `bLeft`, `bRight`, `bResult`); the unary ones `Input`
(`bInput`) and `Result`. An operand that is a constant is that property's value on the function node; an operand that is a path
is bound; an operand that is another operation nests another function. Every other property function is a call; its output is
the property named `Result`, `bResult` or `Output`, or the only Output property.

An operation of constants is folded into one constant when the text is read.

### Utility

`utility 0.5` is a `StateTreeConstantConsideration` with `Constant = 0.5`. A call is the consideration node. `*`, `and` and
`or` are the `Multiply`, `And` and `Or` operands of the list ([below](#conditions-operands-and-indents)).

## Conditions: operands and indents

The engine keeps `EnterConditions`, a transition's `Conditions` and `Considerations` as flat lists. Each item has an
`ExpressionOperand` (`Copy`, `And`, `Or`, and for considerations `Multiply`) joining it to what came before, and an
`ExpressionIndent`. The list is evaluated strictly from left to right with **no precedence**; the indent opens and closes the
parentheses: an item whose next item is `n` levels deeper opens `n` parentheses in front of it, so a group's first item carries
the outer indent. The first item's operand is always `Copy`. At most four levels are allowed (DST5012).

The text's precedence and parentheses are turned into a list this way. Think of the expression as nested groups: a group is
operands joined by operators, left to right; the top group is level 0.

- An item's **indent** is the depth of the innermost group it is in, minus one for each group that starts at this item.
- An item's **operand** is the operator that joins it, or the group it starts, to what comes before.

| Expression | List (operand, indent) |
|---|---|
| `A and B and C` | A(Copy, 0) B(And, 0) C(And, 0) |
| `A and (B or C)` | A(Copy, 0) B(And, 0) C(Or, 1) |
| `A and (B or (C and D))` | A(Copy, 0) B(And, 0) C(Or, 1) D(And, 2) |
| `A or B and C` | A(Copy, 0) B(Or, 0) C(And, 1) |
| `(A or B) and C` | A(Copy, 0) B(Or, 0) C(And, 0) |

The last row shows the one place the text and the list part ways: a group that is the first operand of its chain is flattened
into the chain, because left to right the engine reads it the same way. Decompiling writes the parentheses back where the
precedence needs them. A list whose item has the operand `Copy` after the first has no text form and is read as `and` (DST8058).
`not` is the `bInvert` of one condition; it never covers a group (DST5010).

## Identity

Everything the build creates gets an ID that is the same on every build, so that tools can follow a thing across rebuilds:

`ID = MD5("<asset path>|<key>[|<salt>]")`, the first 16 bytes as a GUID.

| Thing | Key | Salt |
|---|---|---|
| a state | its path `Root/Attack` | |
| a state's parameters struct | the state path | `Parameters` |
| a parameter of a state | `<state path>/Param:<name>` | |
| a transition | `<state path>#<index>` | |
| a task, condition or consideration | `<state path>/Task[1]`, `/EnterCondition[0]`, `/Consideration[0]`, `/Transition[2]/Condition[0]` | |
| a global node | `Global[0]` | |
| a property function | `<owner key>/Fn:<property path>`, such as `Fn:Duration` or `Fn:QueryConfig[0].Value` | |
| the tree's parameters struct | `Parameters` | |
| a tree parameter | `Param:<name>` | |
| a color | `Color:<name>` | |

Event IDs are derived by the engine from the state or transition they belong to.

The engine gives an asset copy all-new IDs (`UStateTreeEditorData::PostDuplicate` calls `DuplicateIDs`). A tool that needs to
follow a thing therefore matches by ID first and then by the state's path together with the node's name (or its role and index),
which is what write-back does. Renaming a state makes it a new state.

Two engine bugs of 5.8 matter here. `DuplicateIDs` trips an `ensure` for every state with a required event, so duplicating such
a tree in the editor shows it. And a copied asset's bindings to event payloads point at event IDs that no longer exist; the
copy fails to compile. Building from the text fixes both; the decompiler repairs such a binding where it can and says so
(DST8006).

## What the text does not hold

| Engine | Why |
|---|---|
| editor data that is a subclass of `UStateTreeEditorData` | the model cannot hold what the subclass adds (DST8001) |
| editor data extensions other than the view settings | would be lost (DST8002) |
| a fixed-size array property that differs from its default | no text form (DST8004) |
| a localized `FText` property value | the text holds plain strings (DST8004) |
| a transition with `ForceSustained` | read as the default, with a warning (DST8004) |
| a transition's selection fallback | no field in the model (DST8008, warning) |
| a binding that reads from a member of an array element | no way to name it (DST8054) |
| a binding into a part of a value whose type only the instance knows (an instanced struct, an object), other than a reference's parameters | no way to name it (DST8054) |
| a property function without a node, a binding into a transition other than its delegate listener | no place in the model (DST8007, DST8008) |
| two sibling states with the same name | the model identifies a state by its path (DST8005) |
