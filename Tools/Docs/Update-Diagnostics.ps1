# Copyright DreamMoon. All Rights Reserved.
#
# Keeps Docs/Diagnostics.md in step with the diagnostic codes in the source.
#
#   Update-Diagnostics.ps1 [-Check] [-Prune] [-Plugin <dir>] [-Doc <file>]
#
# The script scans Source/ (the test module excluded) for every `TEXT("DSTnnnn")` and reads, for each code:
#   - its severity, from the call it is raised in (Error, Warning, Info);
#   - the first message the source gives it: the first string literal of the call that reads like a sentence.
#     A call that builds its message at run time has no literal and is skipped; a code none of whose calls has one
#     keeps the message already in the document (or `(built at run time)` for a new code).
#
# Docs/Diagnostics.md holds one table per thousand between marker comments:
#
#   <!-- dst:begin 1 -->
#   | Code | Severity | Message | Cause and fix |
#   ...
#   <!-- dst:end 1 -->
#
# The script rewrites only what is between the markers. Per code it owns the Code, Severity and Message columns;
# the "Cause and fix" column is written by hand and is carried over by code. A code found in the source and not
# in the document gets a row with the fix `TODO`. A row whose code is no longer in the source is kept (and listed)
# unless -Prune is given, so a code documented before its source is written is not lost.
#
# Run it after changing a diagnostic, then fill in the TODO rows. It is idempotent: a second run changes nothing.
#
#   -Check   write nothing; exit 1 when the document is out of date or has TODO rows (for CI)
#   -Prune   drop the rows whose codes are not in the source
#
# Exit codes: 0 done (with -Check: up to date); 1 with -Check only: out of date or rows still TODO; 2 the document or the source could not be read.

[CmdletBinding()]
param(
    [string]$Plugin = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
    [string]$Doc,
    [switch]$Check,
    [switch]$Prune
)

$ErrorActionPreference = 'Stop'
if (-not $Doc) { $Doc = Join-Path $Plugin 'Docs\Diagnostics.md' }

$ellipsis = [string][char]0x2026
$utf8 = [Text.UTF8Encoding]::new($false)

# ---------------------------------------------------------------- the source
function Get-SourceFiles {
    $sourceDir = Join-Path $Plugin 'Source'
    if (-not (Test-Path -LiteralPath $sourceDir)) { throw "No Source folder under $Plugin." }
    # The language module first, then the compiler, then the editor: the first message a code has is the earliest stage's.
    $rank = { param($f) if ($f.FullName -match '[\\/]DreamStateTreeLang[\\/]') { 0 } elseif ($f.FullName -match '[\\/]DreamStateTreeCompiler[\\/]') { 1 } else { 2 } }
    Get-ChildItem -LiteralPath $sourceDir -Recurse -File -Include '*.cpp', '*.h' |
        Where-Object { $_.FullName -notmatch '[\\/]DreamStateTreeTests[\\/]' } |
        Sort-Object @{ Expression = { & $rank $_ } }, FullName
}

# The index of the `(` that opens the call the code at $At is an argument of; -1 when there is none close by.
function Find-CallOpen([string]$Text, [int]$At) {
    $depth = 0
    $limit = [Math]::Max(0, $At - 400)
    for ($i = $At - 1; $i -ge $limit; $i--) {
        $c = $Text[$i]
        if ($c -eq ')') { $depth++ }
        elseif ($c -eq '(') {
            if ($depth -eq 0) { return $i }
            $depth--
        }
        elseif ($c -eq ';' -or $c -eq '{' -or $c -eq '}') { return -1 }
    }
    return -1
}

# The index just past a string or character literal that starts at $i.
function Skip-Literal([string]$Text, [int]$i) {
    $quote = $Text[$i]
    $i++
    while ($i -lt $Text.Length -and $Text[$i] -ne $quote) { if ($Text[$i] -eq '\') { $i++ }; $i++ }
    return $i + 1
}

# The arguments of the call opened at $Open, split at the commas that are not inside brackets, strings or comments.
# Returns the argument texts and the index of the closing parenthesis.
function Split-CallArguments([string]$Text, [int]$Open) {
    $args_ = [Collections.Generic.List[string]]::new()
    $depth = 0
    $start = $Open + 1
    $i = $Open
    while ($i -lt $Text.Length) {
        $c = $Text[$i]
        if ($c -eq '"' -or $c -eq "'") { $i = Skip-Literal $Text $i; continue }
        if ($c -eq '/' -and $i + 1 -lt $Text.Length -and $Text[$i + 1] -eq '/') {
            while ($i -lt $Text.Length -and $Text[$i] -ne "`n") { $i++ }
            continue
        }
        if ($c -eq '(' -or $c -eq '[' -or $c -eq '{') { $depth++ }
        elseif ($c -eq ')' -or $c -eq ']' -or $c -eq '}') {
            $depth--
            if ($depth -eq 0) {
                $args_.Add($Text.Substring($start, $i - $start))
                return [pscustomobject]@{ Args = $args_; Close = $i }
            }
        }
        elseif ($c -eq ',' -and $depth -eq 1) {
            $args_.Add($Text.Substring($start, $i - $start))
            $start = $i + 1
        }
        $i++
    }
    return [pscustomobject]@{ Args = $args_; Close = $Text.Length - 1 }
}

function ConvertFrom-CppLiteral([string]$Literal) {
    $sb = [Text.StringBuilder]::new()
    for ($i = 0; $i -lt $Literal.Length; $i++) {
        $c = $Literal[$i]
        if ($c -eq '\' -and $i + 1 -lt $Literal.Length) {
            $i++
            switch ($Literal[$i]) {
                'n' { [void]$sb.Append(' ') }
                't' { [void]$sb.Append(' ') }
                default { [void]$sb.Append($Literal[$i]) }
            }
        }
        else { [void]$sb.Append($c) }
    }
    $sb.ToString()
}

# A literal that reads like a message: a few words, not a format fragment such as `%s#%d` or a property name.
function Test-IsMessage([string]$Text) {
    $Text.Length -ge 12 -and $Text -match '\s' -and $Text -match '[A-Za-z]{3}'
}

# The message an argument expression gives: the first sentence-like literal in it, `Expected ..., found ...` for the
# parser's Expected(...) helper, or $null when it is built at run time.
function Get-ArgumentMessage([string]$Argument) {
    if ($Argument -match '^\s*Expected\(') { return "Expected $ellipsis, found $ellipsis." }
    foreach ($lit in [regex]::Matches($Argument, 'TEXT\("((?:[^"\\]|\\.)*)"\)')) {
        $value = ConvertFrom-CppLiteral $lit.Groups[1].Value
        if (Test-IsMessage $value) { return $value }
    }
    return $null
}

function Get-DiagnosticsFromSource {
    $codes = @{}
    foreach ($file in Get-SourceFiles) {
        $text = [IO.File]::ReadAllText($file.FullName)
        foreach ($m in [regex]::Matches($text, 'TEXT\("(DST\d{4})"\)')) {
            $code = $m.Groups[1].Value

            # A diagnostic filled in field by field -- `Refusal.Code = TEXT("DST8102"); Refusal.Message = ...; ` -- is a
            # code too: its message is the next `Message = ...;` statement.
            $lead = $text.Substring([Math]::Max(0, $m.Index - 12), [Math]::Min(12, $m.Index))
            if ($lead -match '\.Code\s*=\s*$') {
                $ahead = $text.Substring($m.Index, [Math]::Min(1200, $text.Length - $m.Index))
                $message = $null
                if ($ahead -match '(?s)\.Message\s*=[^;"]*?(TEXT\("(?:[^"\\]|\\.)*"\))') { $message = Get-ArgumentMessage $Matches[1] }
                # Such records are filed through write-back's Refuse(), which reports them as warnings.
                $severity = 'warning'
                if (-not $codes.ContainsKey($code)) {
                    $codes[$code] = [pscustomobject]@{ Code = $code; Severities = [Collections.Generic.List[string]]::new(); Message = $null; Sites = 0 }
                }
                $entry = $codes[$code]
                $entry.Sites++
                if (-not $entry.Severities.Contains($severity)) { $entry.Severities.Add($severity) }
                if (-not $entry.Message -and $message) { $entry.Message = $message }
                continue
            }

            $open = Find-CallOpen $text $m.Index
            if ($open -lt 0) { continue }
            $split = Split-CallArguments $text $open
            $head = $text.Substring([Math]::Max(0, $open - 60), [Math]::Min(60, $open))
            $name = if ($head -match '([A-Za-z_][\w:.]*)\s*$') { $Matches[1] } else { '' }

            $call = $text.Substring($open, $split.Close - $open + 1)
            # Helpers whose name does not say how bad: write-back's Refuse() files its diagnostics as warnings.
            $severity = if ($name -match '(^|[.:])Refuse$') { 'warning' }
                        elseif ($name -match 'Warning') { 'warning' }
                        elseif ($name -match 'Info') { 'info' }
                        elseif ($name -match 'Error') { 'error' }
                        elseif ($call -match 'bWarning\*/\s*true') { 'warning' }
                        else { 'error' }

            # Where the message is among the arguments: after the code come the span (and, for the *In forms, the file
            # before it), then the message. AddFileError and ReportOnce take the message right after the code.
            $codeIndex = -1
            for ($k = 0; $k -lt $split.Args.Count; $k++) { if ($split.Args[$k] -match 'TEXT\("DST\d{4}"\)') { $codeIndex = $k; break } }
            $offset = if ($name -match 'In$') { 3 } elseif ($name -match 'AddFileError|ReportOnce') { 1 } else { 2 }
            $message = $null
            if ($codeIndex -ge 0 -and $codeIndex + $offset -lt $split.Args.Count) {
                $message = Get-ArgumentMessage $split.Args[$codeIndex + $offset]
            }

            if (-not $codes.ContainsKey($code)) {
                $codes[$code] = [pscustomobject]@{ Code = $code; Severities = [Collections.Generic.List[string]]::new(); Message = $null; Sites = 0 }
            }
            $entry = $codes[$code]
            $entry.Sites++
            if (-not $entry.Severities.Contains($severity)) { $entry.Severities.Add($severity) }
            if (-not $entry.Message -and $message) { $entry.Message = $message }
        }
    }
    $codes
}

# ---------------------------------------------------------------- the document

function Split-Cells([string]$Row) {
    $inner = $Row.Trim()
    if ($inner.StartsWith('|')) { $inner = $inner.Substring(1) }
    if ($inner.EndsWith('|') -and -not $inner.EndsWith('\|')) { $inner = $inner.Substring(0, $inner.Length - 1) }
    [regex]::Split($inner, '(?<!\\)\|') | ForEach-Object { $_.Trim() }
}

function ConvertTo-Cell([string]$Text) {
    # A format specifier is a gap in the message; a bar would end the cell.
    $t = [regex]::Replace($Text, '%(?:lld|llu|ld|d|u|s|f|g)', $ellipsis)
    $t.Replace('|', '\|')
}

function Read-DocumentRows([string[]]$Lines) {
    $rows = @{}
    foreach ($line in $Lines) {
        if ($line -match '^\|\s*(DST\d{4})\s*\|') {
            $cells = @(Split-Cells $line)
            if ($cells.Count -ge 4) {
                $rows[$cells[0]] = [pscustomobject]@{ Code = $cells[0]; Severity = $cells[1]; Message = $cells[2]; Fix = ($cells[3..($cells.Count - 1)] -join ' | ') }
            }
        }
    }
    $rows
}

if (-not (Test-Path -LiteralPath $Doc)) { Write-Error "No document at $Doc."; exit 2 }

$source = Get-DiagnosticsFromSource
$original = [IO.File]::ReadAllText($Doc)
$newline = if ($original.Contains("`r`n")) { "`r`n" } else { "`n" }
$lines = $original -split "`r?`n"
$documented = Read-DocumentRows $lines

$out = [Collections.Generic.List[string]]::new()
$regionsSeen = @()
$added = @(); $kept = @(); $dropped = @(); $todo = @()

$i = 0
while ($i -lt $lines.Count) {
    $line = $lines[$i]
    if ($line -match '^<!--\s*dst:begin\s+(\d)\s*-->\s*$') {
        $thousand = [int]$Matches[1]
        $regionsSeen += $thousand
        $out.Add($line)
        # Skip to the end marker.
        $j = $i + 1
        while ($j -lt $lines.Count -and $lines[$j] -notmatch '^<!--\s*dst:end\s+(\d)\s*-->\s*$') { $j++ }
        if ($j -ge $lines.Count) { Write-Error "dst:begin $thousand has no dst:end."; exit 2 }

        $low = $thousand * 1000
        $high = $low + 999
        $inRange = { param($c) $n = [int]$c.Substring(3); $n -ge $low -and $n -le $high }

        $codes = [Collections.Generic.SortedSet[string]]::new([StringComparer]::Ordinal)
        foreach ($c in $source.Keys) { if (& $inRange $c) { [void]$codes.Add($c) } }
        foreach ($c in $documented.Keys) {
            if (-not (& $inRange $c)) { continue }
            if ($source.ContainsKey($c)) { continue }
            if ($Prune) { $dropped += $c } else { $kept += $c; [void]$codes.Add($c) }
        }

        $out.Add('| Code | Severity | Message | Cause and fix |')
        $out.Add('|---|---|---|---|')
        foreach ($c in $codes) {
            $old = $documented[$c]
            if ($source.ContainsKey($c)) {
                $s = $source[$c]
                $severity = ($s.Severities | Sort-Object { switch ($_) { 'error' { 0 } 'warning' { 1 } default { 2 } } }) -join ', '
                $message = if ($s.Message) { ConvertTo-Cell $s.Message }
                           elseif ($old -and $old.Message -and $old.Message -ne '(built at run time)') { $old.Message }
                           else { '(built at run time)' }
                $fix = if ($old) { $old.Fix } else { $added += $c; 'TODO' }
                if ($fix -eq 'TODO') { $todo += $c }
                $out.Add("| $c | $severity | $message | $fix |")
            }
            else {
                $out.Add("| $c | $($old.Severity) | $($old.Message) | $($old.Fix) |")
            }
        }
        $out.Add($lines[$j])
        $i = $j + 1
        continue
    }
    $out.Add($line)
    $i++
}

$missing = @(1..8 | Where-Object { $regionsSeen -notcontains $_ })
$rootless = @($source.Keys | Where-Object { $regionsSeen -notcontains [int]$_.Substring(3, 1) })

$result = ($out -join $newline)
$changed = $result -ne $original

if ($rootless.Count -gt 0) {
    Write-Warning ("Codes in a thousand the document has no table for: " + ($rootless -join ', '))
}
if ($kept.Count -gt 0) { Write-Host ("Documented but not in the source (kept): " + (($kept | Sort-Object -Unique) -join ', ')) }
if ($dropped.Count -gt 0) { Write-Host ("Dropped: " + (($dropped | Sort-Object -Unique) -join ', ')) }
if ($added.Count -gt 0) { Write-Host ("New rows (fill in the fix): " + ($added -join ', ')) }
$noMessage = @($source.Values | Where-Object { -not $_.Message } | ForEach-Object { $_.Code } | Sort-Object)
if ($noMessage.Count -gt 0) { Write-Host ("Message built at run time: " + ($noMessage -join ', ')) }
Write-Host ("{0} codes in the source, {1} documented; {2} still TODO." -f $source.Count, $documented.Count, @($todo | Sort-Object -Unique).Count)

if ($Check) {
    if ($changed -or $todo.Count -gt 0) { Write-Host 'Docs/Diagnostics.md is out of date.'; exit 1 }
    Write-Host 'Docs/Diagnostics.md is up to date.'
    exit 0
}

if ($changed) {
    [IO.File]::WriteAllText($Doc, $result, $utf8)
    Write-Host "Rewrote $Doc"
}
else {
    Write-Host 'Nothing to change.'
}
exit 0
