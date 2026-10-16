# Copyright DreamMoon. All Rights Reserved.
#
# Creates (or refreshes) the DreamStateTree test host: a minimal code project that builds DreamStateTree on its
# own and runs its automation tests away from any working project.
#
#   <HostDir>/                          copy of Tools/TestHost/Template
#   <HostDir>/Plugins/DreamStateTree    git worktree of this repository on -Branch (daily development)
#   <HostDir>/Source/TP_ThirdPerson     the third-person template's game module, under its own name, so the
#   <HostDir>/Content/Variant_*         StateTree assets that template ships load as they are
#   <CorpusDir>/TP_FirstPerson          the other templates with StateTree assets, each a project of its own:
#   <CorpusDir>/TP_TopDown              their game modules declare structs with the same names as the
#                                       third-person one, and one project cannot hold both
#   <CorpusDir>/<other project>         any other project copied there by hand (a sample such as GASP)
#
# Every corpus project gets Plugins/DreamStateTree as a junction to the host's worktree, so all of them run the
# same code. The corpus is copied from the engine at -EngineRoot; nothing of it is kept in this repository.
# Running the script again re-copies the template and leaves the worktree and the corpus alone (-RefreshCorpus
# copies the corpus again). Build the corpus projects one at a time: they share the plugin's binaries.

[CmdletBinding()]
param(
    [string]$HostDir = 'I:\UnrealProject_Moon\DEV_58\DreamStateTreeTestHost',
    [string]$Branch = 'dev',
    [string]$EngineAssociation = '5.8',
    [string]$EngineRoot = 'I:\UnrealEngine\UE_5.8',
    [string]$CorpusDir = 'I:\UnrealProject_Moon\DEV_58\DreamStateTreeCorpus',
    [switch]$RefreshCorpus
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$template = Join-Path $PSScriptRoot 'Template'

# The engine template that lives in the host, and its variants that ship StateTree assets.
$corpus = @(
    @{ Module = 'TP_ThirdPerson'; Variants = @('Variant_Combat', 'Variant_SideScrolling') }
)
# The templates that become projects of their own under -CorpusDir.
$corpusProjects = @('TP_FirstPerson', 'TP_TopDown')

function Invoke-Git([string[]]$GitArgs) {
    & git @GitArgs
    if ($LASTEXITCODE -ne 0) { throw "git $($GitArgs -join ' ') failed (exit $LASTEXITCODE)" }
}

Write-Host "==> Template -> $HostDir"
New-Item -ItemType Directory -Force -Path $HostDir | Out-Null
$projectPath = Join-Path $HostDir 'DreamStateTreeTestHost.uproject'
Copy-Item -Path (Join-Path $template '*') -Destination $HostDir -Recurse -Force

$pluginDir = Join-Path $HostDir 'Plugins\DreamStateTree'
if (-not (Test-Path $pluginDir)) {
    Write-Host "==> Worktree $Branch -> $pluginDir"
    & git -C $repo rev-parse --verify --quiet "refs/heads/$Branch" | Out-Null
    if ($LASTEXITCODE -ne 0) { Invoke-Git @('-C', $repo, 'branch', $Branch) }
    Invoke-Git @('-C', $repo, 'worktree', 'add', $pluginDir, $Branch)
} else {
    Write-Host "    worktree already there: $pluginDir"
}

$templates = Join-Path $EngineRoot 'Templates'
if (-not (Test-Path -LiteralPath $templates)) { throw "No Templates folder under $EngineRoot. Pass -EngineRoot." }
$modules = [System.Collections.Generic.List[string]]::new()
foreach ($entry in $corpus) {
    $name = $entry.Module
    $sourceFrom = Join-Path $templates "$name\Source\$name"
    $sourceTo = Join-Path $HostDir "Source\$name"
    if ($RefreshCorpus -or -not (Test-Path -LiteralPath $sourceTo)) {
        Write-Host "==> Corpus module $name"
        if (Test-Path -LiteralPath $sourceTo) { Remove-Item -LiteralPath $sourceTo -Recurse -Force }
        Copy-Item -LiteralPath $sourceFrom -Destination $sourceTo -Recurse
        # A project has one primary game module, and it is the host's.
        $moduleCpp = Join-Path $sourceTo "$name.cpp"
        $text = Get-Content -LiteralPath $moduleCpp -Raw -Encoding UTF8
        $text = [regex]::Replace($text, 'IMPLEMENT_PRIMARY_GAME_MODULE\s*\(\s*FDefaultGameModuleImpl\s*,\s*(\w+)\s*,\s*"[^"]*"\s*\)', 'IMPLEMENT_GAME_MODULE(FDefaultGameModuleImpl, $1)')
        Set-Content -LiteralPath $moduleCpp -Value $text -Encoding UTF8 -NoNewline
    }
    foreach ($variant in $entry.Variants) {
        $contentTo = Join-Path $HostDir "Content\$variant"
        if ($RefreshCorpus -or -not (Test-Path -LiteralPath $contentTo)) {
            Write-Host "    content $variant"
            if (Test-Path -LiteralPath $contentTo) { Remove-Item -LiteralPath $contentTo -Recurse -Force }
            Copy-Item -LiteralPath (Join-Path $templates "$name\Content\$variant") -Destination $contentTo -Recurse
        }
    }
    $modules.Add($name)
}

# The template's project file names only the host's module; add the corpus modules after it.
$project = Get-Content -LiteralPath $projectPath -Raw -Encoding UTF8 | ConvertFrom-Json
$project.EngineAssociation = $EngineAssociation
$list = [System.Collections.Generic.List[object]]::new()
foreach ($m in $project.Modules) { $list.Add($m) }
foreach ($name in $modules) {
    if (-not ($list | Where-Object { $_.Name -eq $name })) {
        $list.Add([pscustomobject]@{ Name = $name; Type = 'Runtime'; LoadingPhase = 'Default' })
    }
}
$project.Modules = $list.ToArray()
$project | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $projectPath -Encoding UTF8

function Add-PluginToProject([string]$UProject) {
    $p = Get-Content -LiteralPath $UProject -Raw -Encoding UTF8 | ConvertFrom-Json
    $p.EngineAssociation = $EngineAssociation
    $plugins = [System.Collections.Generic.List[object]]::new()
    if ($p.PSObject.Properties['Plugins']) { foreach ($x in $p.Plugins) { $plugins.Add($x) } }
    if (-not ($plugins | Where-Object { $_.Name -eq 'DreamStateTree' })) {
        $plugins.Add([pscustomobject]@{ Name = 'DreamStateTree'; Enabled = $true })
    }
    $p | Add-Member -NotePropertyName Plugins -NotePropertyValue $plugins.ToArray() -Force
    $p | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $UProject -Encoding UTF8
}

function Add-PluginJunction([string]$ProjectDir) {
    $link = Join-Path $ProjectDir 'Plugins\DreamStateTree'
    if (Test-Path -LiteralPath $link) { return }
    New-Item -ItemType Directory -Force -Path (Split-Path $link) | Out-Null
    New-Item -ItemType Junction -Path $link -Target $pluginDir | Out-Null
    Write-Host "    Plugins\DreamStateTree -> $pluginDir"
}

$projects = [System.Collections.Generic.List[string]]::new()
New-Item -ItemType Directory -Force -Path $CorpusDir | Out-Null
foreach ($name in $corpusProjects) {
    $dir = Join-Path $CorpusDir $name
    if ($RefreshCorpus -and (Test-Path -LiteralPath $dir)) {
        $junction = Join-Path $dir 'Plugins\DreamStateTree'
        if (Test-Path -LiteralPath $junction) { (Get-Item -LiteralPath $junction).Delete() }
        Remove-Item -LiteralPath $dir -Recurse -Force
    }
    if (-not (Test-Path -LiteralPath $dir)) {
        Write-Host "==> Corpus project $name"
        Copy-Item -LiteralPath (Join-Path $templates $name) -Destination $dir -Recurse
    }
    Add-PluginToProject (Join-Path $dir "$name.uproject")
    Add-PluginJunction $dir
    $projects.Add($dir)
}
# Projects copied into the corpus folder by hand: wire them up the same way.
foreach ($dir in Get-ChildItem -LiteralPath $CorpusDir -Directory | Where-Object { $_.Name -notin $corpusProjects }) {
    $uproject = Get-ChildItem -LiteralPath $dir.FullName -Filter '*.uproject' -File | Select-Object -First 1
    if (-not $uproject) { continue }
    Write-Host "==> Corpus project $($dir.Name) (copied by hand)"
    Add-PluginToProject $uproject.FullName
    Add-PluginJunction $dir.FullName
    $projects.Add($dir.FullName)
}

$info = [ordered]@{
    schema            = 1
    repoPath          = $repo
    worktreePath      = $pluginDir
    branch            = $Branch
    engineAssociation = $EngineAssociation
    engineRoot        = $EngineRoot
    corpusModules     = $modules
    corpusProjects    = $projects
}
$info | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $HostDir '.dreamstatetree-testhost.json') -Encoding utf8

Write-Host "==> Done. Build and test with: pwsh -File `"$pluginDir\Tools\dst.ps1`" test"
