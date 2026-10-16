# DreamStateTree

StateTree assets written as text. A `.dtr` file describes one StateTree; DreamStateTree builds it into an ordinary
`UStateTree` asset, writes existing assets out as text, and writes edits made in the StateTree editor back to the
text.

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
            -> Root on Failed [ Delay = 0.5 ]
        }
        State Recover { do Delay(1, 0.3) }
    }
}
```

Status: early development. Unreal Engine 5.8 only.

## Layout

| Path | What |
|---|---|
| `Source/DreamStateTreeLang` | the language: lexer, parser, printer, templates, diagnostics, model (Core only) |
| `Source/DreamStateTreeCompiler` | text to StateTree and back, write-back, command line; the extension interface |
| `Source/DreamStateTreeEditor` | rebuild on save, write-back of editor edits, asset menus |
| `Source/DreamStateTreeTests` | automation tests and their fixtures |
| `Tools/dst.ps1` | build the test host and run the tests |
| `Tools/TestHost` | the test host project and the script that creates it |

The plugin has no runtime module: the assets it builds are plain StateTrees, and a packaged game does not need it.

## Test host

```
pwsh -File Tools/TestHost/New-DreamStateTreeTestHost.ps1
pwsh -File <host>/Plugins/DreamStateTree/Tools/dst.ps1 test
```

The host carries the engine templates' game modules and their StateTree assets as a corpus of real trees; they
are copied from the engine when the host is created and are not part of this repository.
