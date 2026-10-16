# DreamStateTree grammar and canonical format

This is the reference the lexer, the parser and the printer implement (`Source/DreamStateTreeLang/Private/Lang/`). It covers the
syntax only; what a name means (a node type, a schema, a tag, a property) is decided by the later stages and described in
[Language.md](Language.md) and [Mapping.md](Mapping.md).

## Files

| Extension | Document kind | Produces |
|---|---|---|
| `.dtr` | `StateTree(...)` | a StateTree asset |
| `.dth` | `StateTreeLibrary(...)` | nothing; `#include`d by other files |

Text is UTF-8 (a byte order mark is accepted). Line ends are `\n`, `\r\n` or a lone `\r`.

## Lexical structure

- Comments: `// ...` to the end of the line, and `/* ... */` (not nested). Kept as trivia when asked.
- Identifiers: a letter or `_`, then letters, digits and `_`. Any character from U+0080 up that is not a space counts as a
  letter, so Chinese and other names work. Case-sensitive. There are no reserved words at the lexical level: `true`, `State`,
  `on`, `Settings` and the rest are identifiers that the parser recognises by position.
- Names in backticks: `` `Move To` `` is one identifier whose text is what stands between the backticks, spaces and case kept.
  It can be written wherever an identifier can. A name in backticks is never read as a word the parser recognises by position
  (`` `true` `` is a name, not a boolean; `` `do` `` is a name, not a clause). The name is not empty and has no line break or
  backtick in it (DST1007).
- Numbers: `12`, `0.5`, `.5`, `1e-3`, `2.5E+4`. No sign (a leading `-` is the unary operator), no suffix. Anything glued to the
  digits (`2px`, `0x1F`, `1.2.3`, `1.`, `1e`) is one malformed number (DST1004).
- Strings: `"..."` on one line. Escapes: `\"`, `\\`, `\n`, `\t`, `\r`. Any other escape is DST1006; a missing closing quote is
  DST1002.
- Directives: `#` at the start of a line followed by a word: `#include`, `#Region`, `#EndRegion`. Any other word is DST1005.
- Punctuation and operators: `( ) { } [ ] , ; . = -> == != < <= > >= + - * /`. The lexer also reads `: ? % ! && ||`: `!`, `&&`
  and `||` are accepted in place of `not`, `and` and `or` with DST2017; the others have no use and fail where they appear
  (DST2001). A character no token starts with is DST1001.

The words that the parser reads by position:

| Position | Words |
|---|---|
| member of the document | `Template`, `State`, `Subtree`; a name followed by `=` starts a section |
| item of a state body | `State`, `Subtree`, `on`, `if`, `pick`, `utility`, `link`, `do`; `->` |
| after `link` | `asset` (only when a name follows it) |
| after a transition's target | `on`, `if` |
| in an expression | `true`, `false`, `not`, `and`, `or`, `ref` |

## Syntax (EBNF)

`{ x }` is zero or more, `[ x ]` optional, `( a | b )` alternatives, `"x"` a literal word or symbol. Whitespace and comments may
stand between any two tokens, and **line breaks mean nothing**, except that the `"on"` and `"if"` of a `Transition` must
start on the line where the part before them ends (otherwise they begin the state's own `On` or `If` clause).

```
File        = { Include } Document ;                    (* an Include after the document is DST2006 *)
Include     = "#include" String ;
Document    = DocKind "(" [ Arguments ] ")" "{" { Member } "}" ;
DocKind     = "StateTree" | "StateTreeLibrary" ;

Member      = Section | Template | State | Region ;
Section     = Name "=" "{" { Entry | Region } "}" ;
Entry       = Assign | Decl | ValueEntry ;
Assign      = Name "=" Expr [ Meta ] ";" ;              (* Settings, Globals, Colors *)
Decl        = Type Name [ "=" Expr ] [ Meta ] ";" ;     (* Parameters *)
ValueEntry  = Expr ";" ;                                (* Tags *)
Template    = "Template" Name [ "(" [ TParam { "," TParam } ] ")" ] "=" Expr ";" ;
TParam      = Name [ "=" Expr ] ;
Region      = "#Region" String | "#EndRegion" ;

State       = ( "State" | "Subtree" ) Name [ "(" [ SParam { "," SParam } ] ")" ] [ Meta ] "{" { Item } "}" ;
SParam      = Type Name [ "=" Expr ] ;                  (* a Subtree only; a State with parameters is DST2019 *)
Item        = Clause | State | Region ;                 (* a Subtree below the document level is DST2020 *)

Clause      = On | If | Pick | Utility | Link | Do | Transition ;
On          = "on" Postfix ;                            (* Tag, Tag(Payload), or a tag string *)
If          = "if" Expr ;
Pick        = "pick" Name ;
Utility     = "utility" Expr ;
Link        = "link" [ "asset" ] Postfix ;              (* Name, Name(Args), Path(Game, "Rel")(Args) *)
Do          = "do" Task { "," Task } ;
Task        = [ Name "=" ] Postfix ;                    (* a call with its [settings], or a name or path *)
Transition  = "->" Postfix [ "on" Postfix ] [ "if" Expr ] [ Meta ] ;

Meta        = "[" [ MetaEntry { ";" MetaEntry } [ ";" ] ] "]" ;
MetaEntry   = Name [ "=" Expr ] ;                       (* no value: a flag, meaning true *)

Type        = Name [ "<" Type { "," Type } ">" ] ;      (* Array<float>, Class<Actor> *)

Expr        = Binary(1) ;
Binary(p)   = Unary { Op(q >= p) Binary(q + 1) } ;      (* left-associative; the precedences are below *)
Unary       = ( "not" | "!" | "-" ) Unary | Postfix ;
Postfix     = Primary { "." Name | "(" [ Arguments ] ")" [ Meta ] } ;
Primary     = Number | String | "true" | "false" | Name
            | "ref" Postfix                             (* the operand must be a path *)
            | "(" Arguments ")"                         (* one unnamed item and no trailing comma: grouping; else a tuple *)
            | "[" [ Expr { "," Expr } [ "," ] ] "]" ;   (* list *)
Arguments   = Argument { "," Argument } [ "," ] ;
Argument    = [ Name ( "=" | "->" ) ] Expr ;            (* positional; named; output binding *)

Name        = Identifier | BacktickName ;
```

Binary operators, loosest first:

| Level | Operators |
|---|---|
| 1 | `or`, `\|\|` |
| 2 | `and`, `&&` |
| 3 | `==`, `!=` |
| 4 | `<`, `<=`, `>`, `>=` |
| 5 | `+`, `-` |
| 6 | `*`, `/` |

The unary operators `not`, `!` and `-` bind tighter than all of them. `not` is the operator only when an operand can follow; in
`Tell(not)` it is a name.

### Notes

- **Where a clause ends.** A clause ends where the next item begins: `->`, an unquoted clause word (`on`, `if`, `pick`,
  `utility`, `link`, `do`), `State` or `Subtree` followed by a name, a string or `{`, a directive, or the `}` that closes the
  body. At the top level of a clause, an unquoted clause word where an operand is expected is the next clause and the expression
  before it is unfinished (DST2001). Inside brackets, and in backticks, a clause word is a plain name. Joining conditions with
  `and` / `or` and tasks with `,` is what keeps a clause going; nothing separates two clauses.
- **A transition's target and trigger** are `Postfix` expressions: a name, a path (`Idle.Phone`), a call (`Hit(Payload)`,
  `Delegate(Combo.Succeeds)`) or a string. After the target come `on` and `if` in that order; the `if` takes a full `Expr`.
- **Brackets after a call.** `[ ... ]` straight after the `)` of a call is that call's node settings, except for the call at the
  very end of a transition clause. There the last bracket is always the transition's options; settings for that call go in
  front of it (`C() [ Name = "x" ] [ Delay = 1 ]`) or the call goes in parentheses. A bracket is read only after a call; a
  bracket after a name or a path is not part of the expression.
- **Sections and entries.** An item of a document that is `Name =` starts a section whatever the name; which names are sections
  is the checker's business (DST4013). An entry of a section is a `Decl` when it starts with a type and then a name, an
  `Assign` when it starts with a name and `=`, a `ValueEntry` otherwise. A missing `;` is DST2010.
- **Argument rules.** A positional argument after a named one is DST2021; a name twice is DST2023. A document header takes named
  arguments only (DST2005).
- **Tuples.** `(a, b)`, `(X = 1)`, `(a,)` and `()` are tuples; `(a)` is a parenthesized expression, kept as written so that the
  printer can give the parentheses back.
- **Task items.** The name before `=` is the node's binding name. `do Combo = ComboAttack()` has one task; `do A(), B()` has two.
- **Recovery.** The parser keeps going after an error: a failed section entry skips to the next `;` at its depth, a failed item of
  a state skips to the next clause keyword, `State` or `}`, a failed document member skips to the next member, a broken argument
  or list item skips to the next `,`. A file with errors still yields what parsed, and one run reports as many problems as it
  can. The end of the file inside a bracket is one error (DST2024), at the bracket.
- **Nesting.** States and expressions nest to a limited depth (DST2001 when exceeded). Types nest four levels.

## Trivia

The parser keeps comments and blank lines when asked to, because the formatter and write-back print them back:

- A comment on its own line above a statement, a clause, an entry, an argument or a list item is that node's *leading*
  comment.
- A comment on the node's last line, after its `;` or `,`, is its *trailing* comment.
- Comments after the last child, before the closing bracket, belong to the container.
- Comments anywhere else (between a key and its `=`, inside `[ ]`, inside a parameter list) are swept up into the nearest
  child's leading comments.
- Comments above the document are its leading comments; comments after the closing `}` are moved inside it.
- The number of blank lines above a node (before its first leading comment) is kept, as at most one.

## Canonical format

`dst.ps1 fmt` prints this form. The printer's decisions depend on the tree, on the comments and blank lines, and, when the call
asks for the author's layout (write-back does), on which calls and cards were broken over lines in the source. They never depend
on the exact spacing of the source, so formatting twice gives the same text.

### Layout

- Indentation is 4 spaces. Lines end in the file's own line end (`fmt -Write` keeps it; the default is `\n`). The file ends with
  one line end.
- The header is on one line. The `{` and `}` of the document are on lines of their own.
- The document's members go in the order written: sections, then templates, then states; a blank line separates different kinds
  of member, every top-level state and subtree from the next, and any member that spans several lines from its neighbours.
  Adjacent one-line sections and templates stay together.
- A state's `{` follows its name, and the state's settings, on the same line.
- Statements of a block stay in the order written. The printer never moves a clause, a child state or a region.
- Blank lines the author wrote are kept, several as one; none stands above the first statement of a block. Inside a state or a
  section they are the author's alone. The printer adds blank lines only between members of the document.
- A run of statements without a blank line or a leading comment between them that are one-line sections, entries, or one-line
  cards of the same kind is padded so that their `=` or `{` line up.

### Sections

- A section is `Name = { entries }` on one line when it fits in the line, with the entries separated by one space; otherwise
  `{` ends the line and each entry takes a line, indented one step, and `}` stands at the section's indent.
- Consecutive sections align their `=`. Inside a broken section the entries of a run align their `=`; `Decl` entries align the
  type column and, when they have a default, the name column.
- An empty section is `Name = {}`.

### Cards

A card prints on one line, `State Name [ settings ] { clause  clause }`, with two spaces between the clauses, when:

- it has no child states, no comments and no regions;
- it has at most three clauses, of which at most two are anything more than a bare `-> Target`;
- the author left no blank line between its clauses (a blank after the `{` does not count);
- it fits in 120 columns.

Consecutive one-line cards pad their heads so that the `{` of each lines up. Any other card is broken: the `{` ends the head
line, the body goes one step in, the `}` stands at the card's indent.

### Clauses and expressions

- `on`, `if`, `pick`, `utility` and `link` print their keyword, a space and the expression. `do` prints its tasks joined by
  `, `. A transition prints `-> target`, then ` on trigger`, then ` if condition`, then ` [ options ]`.
- A clause that does not fit in 120 columns is broken at its brackets and operators. A call, a list or a tuple is broken one
  item per line, one step in, with its closing bracket at the indent of the line the bracket opened on. A chain of `and` (or of
  `or`) takes one operand per line with the operator in front, every line at the column where the chain started. The tasks of a
  `do` take one line each, aligned under the first, all but the last ending in `,`. A transition breaks its target, trigger and
  condition in turn; its options stay on one line.
- **The author's line breaks.** `fmt` and write-back give the printer the source text, and then a call, a list, a tuple or a
  state card that spans several lines in the source stays broken even where it would fit on one line. The decompiler and the
  test corpus print without a source and join everything that fits. Everything else (clauses, `and` / `or` chains, `do` tasks)
  is laid out from scratch.
- Settings and options are always on one line: `[ Key = Value; Flag ]`. An empty one is `[]`.
- Operators are surrounded by single spaces; `not` is followed by one space and `-` by none. Calls print `Name(a, b)`;
  tuples `(a, b)` and lists `[a, b]`; output bindings `Name -> path`; a named argument `Name = value`.
- A name that is not a plain identifier is printed in backticks. Strings print with the escapes `\"`, `\\`, `\n`, `\t`, `\r`.
  A number prints as it was written.
- `#Region "Label"` and `#EndRegion` stand on lines of their own; a block holding one cannot be a one-line card.

### Comments

- Leading comments are printed above their node at the node's indent; trailing comments after the node on its last line; inner
  comments before the closing bracket of their container.
- A node that carries a comment cannot share a line: every container around it is broken.
- `//` comments lose trailing blanks; block comments are kept verbatim.
