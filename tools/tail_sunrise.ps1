<#
.SYNOPSIS
    Live-tail the Sunrise client log into a terminal, filtered to the stat-block investigation's
    events by default and color-coded per event/outcome. Every line the game logs (filtered or
    not) is also mirrored into a plain-text file with sequence numbers, so nothing is lost even
    when the console view is scoped down. Survives log rotation and game restarts. Ctrl+C to stop.

.EXAMPLE
    .\tools\tail_sunrise.ps1
        Shows only the diagnostics this investigation cares about: stat_scan, stat_watch,
        debug_flag_scan, equip_light, ability, char_stats, and any line at error level.

.EXAMPLE
    .\tools\tail_sunrise.ps1 -All
        Shows every event the game logs, including the high-volume netcode/gameplay channels
        (ev=gameplay, ev=actor_policy, ev=transport, ev=dtls_host and friends) that the default
        filter deliberately excludes because they would drown out the stat-scan output.

.EXAMPLE
    .\tools\tail_sunrise.ps1 -Tail 50 -Include 'ev=char_stats','ev=item_state'
        Custom event allowlist instead of the built-in one.
#>
[CmdletBinding()]
param(
    [string]$LogPath = "C:\Users\Tatsuya\Pictures\Destiny2-Unvaulting\bin\x64\Sunrise\logs\sunrise.log",
    [string]$Mirror  = "C:\Users\Tatsuya\Pictures\Destiny2-Unvaulting\bin\x64\Sunrise\logs\sunrise-live.txt",
    [int]$Tail       = 20,
    [switch]$All,
    [string[]]$Include
)

$ErrorActionPreference = 'Stop'

# The events this investigation actually cares about. Everything else the game logs (netcode,
# matchmaking, group/fireteam hosting, DTLS, web-service actions...) is real but not relevant
# here and is high-volume enough during real play to bury the stat-scan output if shown by
# default. -All bypasses this; -Include replaces it with a caller-supplied list.
$script:defaultEvents = @(
    'ev=stat_scan',
    'ev=stat_watch',
    'ev=debug_flag_scan',
    'ev=equip_light',
    'ev=ability',
    'ev=char_stats'
)

$script:pos     = 0
$script:seq     = 0
$script:pending = ''
$enc = New-Object System.Text.UTF8Encoding($false)

function Write-Comment([string]$msg) { Write-Host $msg -ForegroundColor DarkGray }

# Opened with FileShare.ReadWrite so another instance of this script (a stale one left running
# in another window, or a second one started deliberately) never hard-crashes this one just for
# sharing the same mirror path. If the path is still unusable after a couple of tries (e.g. a
# non-sharing process, or a permissions issue), fall back to a PID-suffixed mirror instead of
# refusing to start at all -- a live view is the point, and it should not depend on nothing else
# ever having touched this file.
function Open-Writer([string]$path) {
    for ($attempt = 1; $attempt -le 3; $attempt++) {
        try {
            $stream = [System.IO.FileStream]::new($path, [System.IO.FileMode]::Create,
                [System.IO.FileAccess]::Write, [System.IO.FileShare]::ReadWrite)
            $writer = [System.IO.StreamWriter]::new($stream, $enc)
            $writer.AutoFlush = $true
            return $writer
        } catch {
            if ($attempt -eq 3) { return $null }
            Start-Sleep -Milliseconds 300
        }
    }
    return $null
}

$script:writer = Open-Writer $Mirror
if (-not $script:writer) {
    $fallback = [System.IO.Path]::ChangeExtension($Mirror, $null).TrimEnd('.') + "-$PID.txt"
    Write-Comment "mirror path busy after retries, falling back to: $fallback"
    $script:writer = Open-Writer $fallback
    if (-not $script:writer) {
        throw "Could not open a mirror file at '$Mirror' or the fallback '$fallback'."
    }
    $Mirror = $fallback
}

function Test-Filter([string]$line) {
    if ($All) { return $true }
    if ($line -match ' level=error ') { return $true }
    $events = if ($Include) { $Include } else { $script:defaultEvents }
    foreach ($needle in $events) {
        if ($line.Contains($needle)) { return $true }
    }
    return $false
}

function Get-LineColor([string]$line) {
    # Outcome overrides everything else: a failing line must never read as green just because
    # it also happens to carry `stage=done` (every terminal line does, success or not).
    if ($line -match 'result=(fail|fault\w*|not_found|abort|thread_fail)' -or
        $line -match ' level=error ') {
        return 'Red'
    }
    if ($line -match 'stage=(hit|found|change|code|caller|begin)' -or
        ($line -match 'stage=done' -and $line -notmatch 'result=(fail|fault\w*|not_found|abort)')) {
        return 'Green'
    }
    if ($line -match 'ev=stat_scan')                       { return 'Cyan' }
    if ($line -match 'ev=stat_watch')                      { return 'Magenta' }
    if ($line -match 'ev=char_stats')                      { return 'Yellow' }
    if ($line -match 'ev=(debug_flag_scan|equip_light|ability)') { return 'DarkYellow' }
    return 'Gray'
}

function Emit-Line([string]$line) {
    $line = $line.TrimEnd("`r")
    if ($line -eq '') { return }
    $script:seq++
    $script:writer.WriteLine(('{0:D6}| {1}' -f $script:seq, $line))
    if (-not (Test-Filter $line)) { return }
    Write-Host $line -ForegroundColor (Get-LineColor $line)
}

function Open-Stream {
    try {
        return [System.IO.File]::Open($LogPath, [System.IO.FileMode]::Open,
                [System.IO.FileAccess]::Read,
                [System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete)
    } catch { return $null }
}

function Read-Available($fs) {
    if ($fs.Length -lt $script:pos) {
        $script:pending = ''
        Write-Comment '--- log rotated / recreated; continuing from top ---'
        $script:pos = 0
    }
    $avail = $fs.Length - $script:pos
    if ($avail -le 0) { return }
    $bytes = New-Object byte[] $avail
    $fs.Seek($script:pos, [System.IO.SeekOrigin]::Begin) | Out-Null
    [void]$fs.Read($bytes, 0, $avail)
    $script:pos = $fs.Length
    $script:pending += $enc.GetString($bytes)
    while ($true) {
        $nl = $script:pending.IndexOf("`n")
        if ($nl -lt 0) { break }
        $line = $script:pending.Substring(0, $nl)
        $script:pending = $script:pending.Substring($nl + 1)
        if ($line -ne '') { Emit-Line $line }
    }
}

while (-not (Open-Stream)) {
    Write-Comment "waiting for log: $LogPath"
    Start-Sleep -Milliseconds 1000
}

$fs = Open-Stream
if ($Tail -gt 0 -and $fs.Length -gt 0) {
    $total = $fs.Length
    $take  = [Math]::Min([long]8MB, $total)
    $bytes = New-Object byte[] $take
    $fs.Seek($total - $take, [System.IO.SeekOrigin]::Begin) | Out-Null
    [void]$fs.Read($bytes, 0, $take)
    $tailLines = ($enc.GetString($bytes) -split "`n" | Where-Object { $_ -ne '' } | Select-Object -Last $Tail)
    foreach ($l in $tailLines) { Emit-Line $l.TrimEnd("`r") }
}
$fs.Close()
$fs = Open-Stream
$script:pos = if ($fs) { $fs.Length } else { 0 }
if ($fs) { $fs.Close() }

Write-Comment "tail:   $LogPath"
Write-Comment "mirror: $Mirror  (every line recorded regardless of filter)"
if ($All) {
    Write-Comment 'console: ALL events'
} elseif ($Include) {
    Write-Comment "console: $($Include -join ', ')"
} else {
    Write-Comment "console: $($script:defaultEvents -join ', '), plus any error-level line"
}
Write-Comment 'streaming... Ctrl+C to stop.'

try {
    while ($true) {
        $fs = Open-Stream
        if ($fs) { Read-Available $fs; $fs.Close() }
        Start-Sleep -Milliseconds 300
    }
}
finally {
    try { $script:writer.Flush(); $script:writer.Close() } catch {}
    Remove-Variable pos, seq, pending, writer -Scope Script -ErrorAction SilentlyContinue
}
