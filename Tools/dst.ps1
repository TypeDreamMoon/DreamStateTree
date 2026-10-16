# Copyright DreamMoon. All Rights Reserved.
#
# DreamStateTree command line.
#
#   dst.ps1 make                  build the test host's editor target (the C++)
#   dst.ps1 test [-Preset P] [-Filter F] [-NoBuild]
#                                 make, then run the automation tests headless in the test host. Presets:
#                                 Quick (the language, the compiler's corpus and unit tests, write-back, the
#                                 editor; the default), RoundTrip (every corpus tree decompiled, rebuilt and
#                                 compared as text, as a model and as a compiled asset), Behavior (rebuilt
#                                 trees run side by side with the originals), Spike (the engine-behaviour
#                                 experiments), All. -Filter replaces the preset's tests; several prefixes
#                                 are joined with `+`.
#
#   dst.ps1 build [-File F] [-Root R] [-NoSave] [-Json J]
#                                 compile the .dtr/.dth sources into StateTree assets and save them
#   dst.ps1 check [-File F] [-Root R] [-Json J]
#                                 parse, includes, templates and semantic checks only; nothing is generated
#   dst.ps1 fmt [-File F] [-Root R] [-Write | -Verify] [-Markers] [-Json J]
#                                 print the canonical text; -Write rewrites the files that differ (keeping
#                                 their line ends); -Verify writes nothing and fails on any file not canonical
#   dst.ps1 decompile (-Asset A[,B] | -Under /Game/Folder) [-Out O] [-TagPrefix P[,Q]] [-Stdout [-Markers]] [-Force] [-Json J]
#                                 StateTree assets to text, below the project's DTree/ unless -Out;
#                                 a file that exists and differs is only replaced with -Force; -TagPrefix
#                                 shortens gameplay tags with these prefixes and declares them in a Tags section
#
# build, check, fmt and decompile run `UnrealEditor-Cmd <project> -run=DreamStateTree` on -Project, else the
# project above -File, else the test host, and keep the lines that matter: diagnostics
# (`<file>(<line>,<col>): error DSTnnnn: ...`), the summary, errors and crashes (-Raw shows everything).
# -File is a source (absolute, relative to the current directory, or relative to the project directory);
# a .dth stands for the trees that include it. -Root replaces the project's and the enabled plugins'
# DTree/ folders. -Out and -Json are relative to the project directory. -Json writes every diagnostic as
# JSON for the VS Code extension. fmt and decompile -Stdout print the text on the output stream and their
# status lines on the host; several files' texts come between `===== begin <name> =====` and
# `===== end <name> =====` lines, and -Markers puts those lines around a single text too (a host line
# also reaches a redirected stdout, so a tool reading it needs them). Redirected output is UTF-8.
# The engine log of each run is kept in <project>/Saved/DreamStateTree/.
# The engine is the project's EngineAssociation, looked up among registered source builds and Launcher
# installations; -EngineRoot <dir> names one instead.
# A build writes packages: close any editor that has the project open.
#
# Exit codes
#   make: 0 built, 2 failed.
#   test: 0 green, 1 red (a test failed or an ensure fired), 2 not judged (the build failed, the editor
#         crashed or timed out, or fewer tests ran than the preset expects). Results land in
#         <host>/Saved/DreamStateTreeTests/<time>-<preset>/: the engine's index.json, the run log and summary.md.
#   build, check, fmt, decompile: 0 no errors, 1 errors (fmt -Verify: a file not canonical), 2 the
#         commandlet did not finish (it crashed, timed out, or never printed its summary).

[CmdletBinding()]
param(
    [Parameter(Position = 0)][ValidateSet('make', 'test', 'build', 'check', 'fmt', 'decompile')][string]$Command = 'test',
    [ValidateSet('Quick', 'RoundTrip', 'Behavior', 'Spike', 'All')][string]$Preset = 'Quick',
    [string]$Filter,
    [switch]$NoBuild,
    [string]$Project,
    [string]$EngineRoot,
    [int]$TimeoutMinutes = 0,
    [string]$File,
    [string[]]$Root,
    [switch]$Write,
    [switch]$Verify,
    [string[]]$Asset,
    [string]$Under,
    [string]$Out,
    [string[]]$TagPrefix,
    [switch]$Stdout,
    [string]$Json,
    [switch]$NoSave,
    [switch]$Force,
    [switch]$Raw,
    [switch]$Markers
)

$ErrorActionPreference = 'Stop'

# A redirected stdout is otherwise written in the console's code page, which mangles Chinese names. An
# interactive console keeps its own setting.
if ([Console]::IsOutputRedirected) { [Console]::OutputEncoding = [Text.UTF8Encoding]::new($false) }

# MinTests is the least a preset runs: fewer means the filter or the module is broken, not a green run.
$Presets = @{
    Quick     = @{ Filter = 'DreamStateTree.Lang.+DreamStateTree.Compiler.+DreamStateTree.WriteBack.+DreamStateTree.Editor.'; MinTests = 50; Timeout = 30 }
    RoundTrip = @{ Filter = 'DreamStateTree.RoundTrip.'; MinTests = 1; Timeout = 60 }
    Behavior  = @{ Filter = 'DreamStateTree.Behavior.'; MinTests = 1; Timeout = 60 }
    Spike     = @{ Filter = 'DreamStateTree.Spike.'; MinTests = 1; Timeout = 30 }
    All       = @{ Filter = 'DreamStateTree.'; MinTests = 100; Timeout = 90 }
}

function Write-Step([string]$Text) { Write-Host "==> $Text" -ForegroundColor Cyan }
function Write-Note([string]$Text) { Write-Host "    $Text" }
function Write-Problem([string]$Text) { Write-Host "!!  $Text" -ForegroundColor Yellow }

function Find-HostProject {
    if ($Project) { return (Resolve-Path $Project).Path }
    # The plugin normally runs from the host's worktree: <host>/Plugins/DreamStateTree/Tools.
    $candidate = Get-ChildItem -Path (Join-Path $PSScriptRoot '..\..\..') -Filter '*.uproject' -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -eq 'DreamStateTreeTestHost.uproject' } | Select-Object -First 1
    if ($candidate) { return $candidate.FullName }
    $default = 'I:\UnrealProject_Moon\DEV_58\DreamStateTreeTestHost\DreamStateTreeTestHost.uproject'
    if (Test-Path $default) { return $default }
    throw 'No test host found. Create it with Tools/TestHost/New-DreamStateTreeTestHost.ps1, or pass -Project.'
}

function Find-ProjectAbove([string]$Path) {
    $dir = Split-Path -Parent $Path
    while ($dir) {
        $found = Get-ChildItem -LiteralPath $dir -Filter '*.uproject' -File -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($found) { return $found.FullName }
        $parent = Split-Path -Parent $dir
        if ($parent -eq $dir) { break }
        $dir = $parent
    }
    return $null
}

function Resolve-EngineRoot([string]$UProject) {
    $assoc = (Get-Content -LiteralPath $UProject -Raw -Encoding UTF8 | ConvertFrom-Json).EngineAssociation
    $root = $EngineRoot
    if (-not $root) {
        $builds = Get-ItemProperty 'HKCU:\Software\Epic Games\Unreal Engine\Builds' -ErrorAction SilentlyContinue
        $root = if ($builds) { $builds.$assoc } else { $null }
        if (-not $root) {
            # A launcher install of another project's engine.
            $launcher = Get-ItemProperty "HKLM:\SOFTWARE\EpicGames\Unreal Engine\$assoc" -ErrorAction SilentlyContinue
            if ($launcher) { $root = $launcher.InstalledDirectory }
        }
        if (-not $root) {
            $manifest = Join-Path $env:ProgramData 'Epic\UnrealEngineLauncher\LauncherInstalled.dat'
            if (Test-Path -LiteralPath $manifest) {
                $installs = (Get-Content -LiteralPath $manifest -Raw -Encoding UTF8 | ConvertFrom-Json).InstallationList
                $install = $installs | Where-Object { $_.AppName -eq "UE_$assoc" } | Select-Object -First 1
                if ($install) { $root = $install.InstallLocation }
            }
        }
    }
    if (-not $root) { throw "Engine association $assoc is not registered." }
    # Only what building needs: a source build gets its editor from the first `make`.
    foreach ($required in @('Engine\Build\Build.version', 'Engine\Build\BatchFiles\Build.bat')) {
        if (-not (Test-Path -LiteralPath (Join-Path $root $required) -PathType Leaf)) {
            throw "Engine $root is missing $required. Pass -EngineRoot to select a complete installation."
        }
    }
    return $root.Replace('/', '\')
}

# -File relative to the current directory, when it names a file there; else as given (the commandlet
# reads it relative to the project directory).
$sourceFile = $File
if ($File -and (Test-Path -LiteralPath $File -PathType Leaf)) { $sourceFile = (Resolve-Path -LiteralPath $File).Path }

$uproject = if ($Command -in 'make', 'test') { Find-HostProject }
elseif ($Project) { (Resolve-Path $Project).Path }
elseif ($sourceFile -and [IO.Path]::IsPathRooted($sourceFile) -and (Find-ProjectAbove $sourceFile)) { Find-ProjectAbove $sourceFile }
else { Find-HostProject }
$projectDir = Split-Path $uproject
$hostName = [IO.Path]::GetFileNameWithoutExtension($uproject)
$engineRoot = Resolve-EngineRoot $uproject
$editorCmd = Join-Path $engineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'

function Invoke-Build {
    Write-Step "Building ${hostName}Editor"
    $log = Join-Path $projectDir 'Saved\DreamStateTreeTests\build.log'
    New-Item -ItemType Directory -Force -Path (Split-Path $log) | Out-Null
    $buildBat = Join-Path $engineRoot 'Engine\Build\BatchFiles\Build.bat'
    & $buildBat "${hostName}Editor" Win64 Development "-Project=$uproject" -WaitMutex -NoHotReloadFromIDE *> $log
    $code = $LASTEXITCODE
    $errors = @(Select-String -LiteralPath $log -Pattern 'error C\d+|error LNK\d+|error :|\): error' | Select-Object -First 15 | ForEach-Object { $_.Line.Trim() })
    if ($code -ne 0) {
        Write-Problem "Build failed (exit $code); log: $log"
        foreach ($e in $errors) { Write-Note $e }
        return $false
    }
    Write-Note (Select-String -LiteralPath $log -Pattern 'Result: |Total execution time' | ForEach-Object { $_.Line.Trim() })
    return $true
}

function Get-Digest([string]$RunDir, [int]$Floor, [bool]$TimedOut, $ExitCode, [double]$Seconds) {
    $runLog = Join-Path $RunDir 'run.log'
    $logLines = if (Test-Path -LiteralPath $runLog) { @(Get-Content -LiteralPath $runLog -Encoding utf8) } else { @() }
    $infra = @()
    $crash = @($logLines | Where-Object { $_ -match '=== Critical error: ===|Fatal error!|Unhandled Exception|Assertion failed:' } | Select-Object -First 5)
    if ($crash.Count -gt 0) { $infra += 'The editor crashed:'; $infra += $crash }
    if ($TimedOut) { $infra += 'The run timed out.' }
    $ensures = @($logLines | Where-Object { $_ -match 'Ensure condition failed' } | Select-Object -Unique)
    $loadFailures = @($logLines | Where-Object { $_ -match "Plugin 'DreamStateTree' failed to load|Unable to load module.*DreamStateTree" } | Select-Object -First 3)
    if ($loadFailures.Count -gt 0) { $infra += 'The plugin did not load:'; $infra += $loadFailures }

    $tests = @()
    $indexPath = Join-Path $RunDir 'index.json'
    if (Test-Path -LiteralPath $indexPath) {
        $index = Get-Content -LiteralPath $indexPath -Raw -Encoding utf8 | ConvertFrom-Json
        foreach ($t in @($index.tests)) {
            $errors = @(); $infos = @()
            foreach ($entry in @($t.entries)) {
                if ($entry.event.type -eq 'Error') { $errors += [string]$entry.event.message }
                elseif ($entry.event.type -eq 'Info') { $infos += [string]$entry.event.message }
            }
            $tests += [pscustomobject]@{ Path = [string]$t.fullTestPath; State = [string]$t.state; Duration = [double]$t.duration; Errors = $errors; Infos = $infos }
        }
    } else {
        $infra += "No index.json in $RunDir; the tests did not report."
    }

    $passed = @($tests | Where-Object { $_.State -eq 'Success' })
    $failed = @($tests | Where-Object { $_.State -eq 'Fail' })
    $incomplete = @($tests | Where-Object { $_.State -notin 'Success', 'Fail', 'Skipped' })
    # The engine exits with -1 when a test failed or logged an error: a red run, not a broken one. Any other
    # exit, or -1 with nothing failed, means the editor stopped for a reason the report does not show.
    $redExit = $ExitCode -eq -1 -and ($failed.Count -gt 0 -or $ensures.Count -gt 0)
    if (-not $TimedOut -and ($null -eq $ExitCode -or ($ExitCode -ne 0 -and -not $redExit))) { $infra += "The editor did not exit successfully (exit $ExitCode)." }
    if ($incomplete.Count -gt 0) { $infra += "$($incomplete.Count) test(s) did not finish: $($incomplete.Path -join ', ')." }
    $ran = $passed.Count + $failed.Count
    if ($ran -lt $Floor) { $infra += "Only $ran test(s) ran; expected at least $Floor. Did DreamStateTreeTests load?" }

    $code = if ($infra.Count -gt 0) { 2 } elseif ($failed.Count -gt 0 -or $ensures.Count -gt 0) { 1 } else { 0 }
    $word = switch ($code) { 0 { 'green' } 1 { 'red' } default { 'not judged' } }

    $summary = @("# DreamStateTree tests -- $word", '')
    $summary += ('Ran {0}: {1} passed, {2} failed; {3:N0} s in the editor.' -f $ran, $passed.Count, $failed.Count, $Seconds)
    if ($infra.Count -gt 0) { $summary += ''; $summary += '## Not judged'; $summary += @($infra | ForEach-Object { "- $_" }) }
    if ($failed.Count -gt 0) {
        $summary += ''; $summary += '## Failed'
        foreach ($t in $failed) { foreach ($e in ($t.Errors | Select-Object -First 5)) { $summary += "- $($t.Path): $e" } }
    }
    if ($ensures.Count -gt 0) { $summary += ''; $summary += '## Ensures'; $summary += @($ensures | ForEach-Object { "- $($_.Trim())" }) }
    $summary += ''; $summary += '## Tests'
    foreach ($t in $tests) {
        $summary += ('- {0} {1} ({2:N0} ms)' -f $t.State, $t.Path, ($t.Duration * 1000.0))
        foreach ($i in ($t.Infos | Select-Object -First 40)) { $summary += "    - $i" }
    }
    Set-Content -LiteralPath (Join-Path $RunDir 'summary.md') -Value ($summary -join "`n") -Encoding utf8
    Write-Step "Tests: $word"
    foreach ($line in $summary | Select-Object -Skip 2) { if ($line) { Write-Note $line } }
    return $code
}

function Invoke-Tests {
    $spec = $Presets[$Preset]
    $runFilter = if ($Filter) { $Filter.Trim() } else { $spec.Filter }
    if ($runFilter -match "[,;'`"]") { throw "The filter may not hold commas, semicolons or quotes: $runFilter" }
    $timeout = if ($TimeoutMinutes -gt 0) { $TimeoutMinutes } else { $spec.Timeout }
    $runDir = Join-Path $projectDir ('Saved\DreamStateTreeTests\{0}-{1}' -f (Get-Date -Format 'yyyyMMdd-HHmmss'), $Preset)
    New-Item -ItemType Directory -Force -Path $runDir | Out-Null
    $runLog = Join-Path $runDir 'run.log'
    # No value may end in a backslash: the C runtime reads \" as a quote.
    $parts = @(
        "`"$uproject`"",
        "-ExecCmds=`"Automation RunTests $runFilter`"",
        '-TestExit="Automation Test Queue Empty"',
        "-ReportExportPath=`"$($runDir.TrimEnd('\'))`"",
        "-abslog=`"$runLog`"",
        '-unattended', '-nopause', '-nosplash', '-NoSound', '-nullrhi', '-NoLiveCoding'
    )
    $argLine = $parts -join ' '
    Set-Content -LiteralPath (Join-Path $runDir 'command.txt') -Value "`"$editorCmd`" $argLine" -Encoding utf8
    Write-Step "Running $runFilter"
    Write-Note "log: $runLog"
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $timedOut = $false
    $proc = Start-Process -FilePath $editorCmd -ArgumentList $argLine -WorkingDirectory $projectDir -NoNewWindow -PassThru `
        -RedirectStandardOutput (Join-Path $runDir 'stdout.txt') -RedirectStandardError (Join-Path $runDir 'stderr.txt')
    $null = $proc.Handle
    if (-not $proc.WaitForExit($timeout * 60 * 1000)) {
        $timedOut = $true
        Write-Problem "Still running after $timeout minutes: killing the editor."
        try { $proc.Kill($true) } catch { }
        $null = $proc.WaitForExit(60000)
    }
    $watch.Stop()
    $exitCode = if ($timedOut) { $null } else { $proc.ExitCode }
    $floor = if ($Filter) { 1 } else { $spec.MinTests }
    return (Get-Digest -RunDir $runDir -Floor $floor -TimedOut $timedOut -ExitCode $exitCode -Seconds $watch.Elapsed.TotalSeconds)
}

# ---------------------------------------------------------------- build / check / fmt / decompile

function Get-EditorsOnProject([string]$UProject) {
    # Every UnrealEditor*.exe whose command line names this .uproject.
    $needle = $UProject.Replace('/', '\').ToLowerInvariant()
    foreach ($p in @(Get-CimInstance -ClassName Win32_Process -Filter "Name LIKE 'UnrealEditor%'" -ErrorAction SilentlyContinue)) {
        $line = [string]$p.CommandLine
        if ($line -and $line.Replace('/', '\').ToLowerInvariant().Contains($needle)) { $p }
    }
}

function Format-Switch([string]$Name, [string]$Value) {
    # One argument of the command line, quoted. A value may not end in a backslash (the C runtime reads \"
    # as a quote) nor hold a quote.
    if ($Value.Contains('"')) { throw "-$Name may not hold a quote: $Value" }
    return "-$Name=`"$($Value.TrimEnd('\'))`""
}

$script:CommandletExit = 2

function Invoke-Commandlet {
    $arguments = @()
    switch ($Command) {
        'check' { $arguments += '-Check' }
        'fmt' { $arguments += '-Format' }
        'decompile' { $arguments += '-Decompile' }
    }
    if ($sourceFile) { $arguments += Format-Switch 'File' $sourceFile }
    if ($Root) { $arguments += Format-Switch 'Root' (($Root | ForEach-Object { $_.TrimEnd('\') }) -join ',') }
    if ($Write) { $arguments += '-Write' }
    if ($Verify) { $arguments += '-Verify' }
    foreach ($a in @($Asset)) { if ($a) { $arguments += Format-Switch 'Asset' $a } }
    if ($Under) { $arguments += Format-Switch 'Under' $Under }
    if ($Out) { $arguments += Format-Switch 'Out' $Out }
    if ($TagPrefix) { $arguments += Format-Switch 'TagPrefix' ($TagPrefix -join ',') }
    if ($Stdout) { $arguments += '-Stdout' }
    if ($Json) { $arguments += Format-Switch 'Json' $Json }
    if ($NoSave) { $arguments += '-NoSave' }
    if ($Force) { $arguments += '-Force' }

    $writesAssets = $Command -eq 'build'
    if ($writesAssets -and @(Get-EditorsOnProject $uproject).Count -gt 0) {
        Write-Problem "An editor has $uproject open; a build that saves packages will fight it over them."
    }

    $runDir = Join-Path $projectDir 'Saved\DreamStateTree'
    New-Item -ItemType Directory -Force -Path $runDir | Out-Null
    $stamp = '{0}-{1}' -f $Command, (Get-Date -Format 'yyyyMMdd-HHmmss')
    $log = Join-Path $runDir "$stamp.log"
    $stdoutFile = Join-Path $runDir "$stamp.stdout.txt"
    $stderrFile = Join-Path $runDir "$stamp.stderr.txt"
    # The log goes to a file, not to stdout: the engine's own -stdout switch is also the commandlet's
    # "print the decompiled text" switch, and stdout carries only the text this way.
    $parts = @("`"$uproject`"", '-run=DreamStateTree') + $arguments + @(
        "-abslog=`"$log`"", '-unattended', '-nopause', '-nosplash', '-NoSound', '-nullrhi', '-NoLiveCoding')
    $argLine = $parts -join ' '
    Write-Verbose "`"$editorCmd`" $argLine"

    $proc = Start-Process -FilePath $editorCmd -ArgumentList $argLine -WorkingDirectory $projectDir -NoNewWindow -PassThru `
        -RedirectStandardOutput $stdoutFile -RedirectStandardError $stderrFile
    $null = $proc.Handle
    $timedOut = $false
    $limit = if ($TimeoutMinutes -gt 0) { $TimeoutMinutes * 60 * 1000 } else { -1 }
    if (-not $proc.WaitForExit($limit)) {
        $timedOut = $true
        try { $proc.Kill($true) } catch { }
        $null = $proc.WaitForExit(60000)
    }
    $exitCode = if ($timedOut) { $null } else { $proc.ExitCode }

    $stdoutLines = if (Test-Path -LiteralPath $stdoutFile) { @(Get-Content -LiteralPath $stdoutFile -Encoding utf8) } else { @() }
    $logLines = if (Test-Path -LiteralPath $log) { @(Get-Content -LiteralPath $log -Encoding utf8) } else { @() }

    # fmt and decompile -Stdout print text: that goes on the output stream, everything else on the host.
    $printsText = ($Command -eq 'fmt' -and -not $Write -and -not $Verify) -or ($Command -eq 'decompile' -and $Stdout)
    $status = if ($printsText) { { param($Line) Write-Host $Line } } else { { param($Line) Write-Output $Line } }

    if ($Raw) {
        $stdoutLines | ForEach-Object { Write-Output $_ }
        $logLines | ForEach-Object { & $status $_ }
    } else {
        # The text blocks, between the commandlet's markers; a single one without them unless -Markers.
        $blocks = [Collections.Generic.List[object]]::new()
        $current = $null
        foreach ($line in $stdoutLines) {
            if ($line -match '^===== begin (.*) =====$') { $current = [pscustomobject]@{ Name = $Matches[1]; Lines = [Collections.Generic.List[string]]::new() }; continue }
            if ($line -match '^===== end (.*) =====$') { if ($current) { $blocks.Add($current); $current = $null }; continue }
            if ($current) { $current.Lines.Add($line) }
        }
        $marked = $Markers -or $blocks.Count -gt 1
        foreach ($block in $blocks) {
            if ($marked) { Write-Output "===== begin $($block.Name) =====" }
            $block.Lines | ForEach-Object { Write-Output $_ }
            if ($marked) { Write-Output "===== end $($block.Name) =====" }
        }

        # The commandlet's own lines in full; from the rest of the engine only errors and crashes.
        foreach ($line in $logLines) {
            $clean = ($line -replace '^\[[^\]]*\]\[\s*\d+\]', '').TrimEnd()
            if ($clean -match '^LogDreamStateTreeCommandlet: (?:(?:Display|Warning|Error|Log): )?(.*)$') { & $status $Matches[1]; continue }
            if ($clean -match '=== Critical error|Fatal error|Unhandled Exception|Assertion failed|: Error: |DST\d{4}') { & $status $clean }
        }
    }

    $crashed = @($logLines + $stdoutLines | Where-Object { $_ -match '=== Critical error: ===|Fatal error!|Unhandled Exception|Assertion failed:' }).Count -gt 0
    $finished = @($logLines | Where-Object { $_ -match 'LogDreamStateTreeCommandlet: (?:Display: )?DreamStateTree( \w+)?: ' }).Count -gt 0
    $script:CommandletExit = 0
    if ($timedOut) { Write-Problem "Still running after $TimeoutMinutes minutes: the editor was killed. Log: $log"; $script:CommandletExit = 2 }
    elseif ($crashed) { Write-Problem "The editor crashed. Log: $log"; $script:CommandletExit = 2 }
    elseif (-not $finished) { Write-Problem "The commandlet did not finish (exit $exitCode). Log: $log"; $script:CommandletExit = 2 }
    elseif ($exitCode -ne 0) { $script:CommandletExit = 1 }
}

# ---------------------------------------------------------------- main

switch ($Command) {
    'make' { if (Invoke-Build) { exit 0 } else { exit 2 } }
    'test' {
        if (-not $NoBuild) { if (-not (Invoke-Build)) { exit 2 } }
        exit (Invoke-Tests)
    }
    default {
        # The function writes its lines to the output stream; its verdict comes back through a variable.
        Invoke-Commandlet
        exit $script:CommandletExit
    }
}
