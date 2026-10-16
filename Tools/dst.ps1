# Copyright DreamMoon. All Rights Reserved.
#
# DreamStateTree command line.
#
#   dst.ps1 make                  build the test host's editor target (the C++)
#   dst.ps1 test [-Preset P] [-Filter F] [-NoBuild]
#                                 make, then run the automation tests headless in the test host. Presets:
#                                 Spike (the engine-behaviour experiments; the default for now), All.
#                                 -Filter replaces the preset's tests.
#
# The engine is the project's EngineAssociation, looked up among registered source builds and Launcher
# installations; -EngineRoot <dir> names one instead.
#
# Exit codes
#   make: 0 built, 2 failed.
#   test: 0 green, 1 red (a test failed or an ensure fired), 2 not judged (the build failed, the editor
#         crashed or timed out, or fewer tests ran than the preset expects). Results land in
#         <host>/Saved/DreamStateTreeTests/<time>-<preset>/: the engine's index.json, the run log and summary.md.

[CmdletBinding()]
param(
    [Parameter(Position = 0)][ValidateSet('make', 'test')][string]$Command = 'test',
    [ValidateSet('Spike', 'All')][string]$Preset = 'Spike',
    [string]$Filter,
    [switch]$NoBuild,
    [string]$Project,
    [string]$EngineRoot,
    [int]$TimeoutMinutes = 0
)

$ErrorActionPreference = 'Stop'

# A redirected stdout is otherwise written in the console's code page, which mangles Chinese names. An
# interactive console keeps its own setting.
if ([Console]::IsOutputRedirected) { [Console]::OutputEncoding = [Text.UTF8Encoding]::new($false) }

# MinTests is the least a preset runs: fewer means the filter or the module is broken, not a green run.
$Presets = @{
    Spike = @{ Filter = 'DreamStateTree.Spike.'; MinTests = 1; Timeout = 30 }
    All   = @{ Filter = 'DreamStateTree.'; MinTests = 1; Timeout = 60 }
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

$uproject = Find-HostProject
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


# ---------------------------------------------------------------- main

switch ($Command) {
    'make' { if (Invoke-Build) { exit 0 } else { exit 2 } }
    'test' {
        if (-not $NoBuild) { if (-not (Invoke-Build)) { exit 2 } }
        exit (Invoke-Tests)
    }
}
