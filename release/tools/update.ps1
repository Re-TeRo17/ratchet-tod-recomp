<#
  update.ps1 - Update.bat: checks GitHub for a newer release of the port and installs it over this folder.
  Only the port's own files are replaced (tod.exe, the dlls, the scripts, the docs, tod.default.ini).
  Never touched: tod.ini (your settings), gamedata\ (saves), cache\ (shaders), game\ and elf\ (your game), mods\, log\.
  Test without GitHub: update.ps1 -Zip <local zip> -Tag v9.9
#>
param([string]$Zip = '', [string]$Tag = '')
$ErrorActionPreference = 'Stop'
$ThisVersion = '1.1'
$root = Split-Path $PSScriptRoot -Parent
$api = 'https://api.github.com/repos/Re-TeRo17/ratchet-tod-recomp/releases/latest'
$page = 'https://github.com/Re-TeRo17/ratchet-tod-recomp/releases'

function Get-Parts([string]$v) {
    $m = [regex]::Match($v, '\d+(\.\d+)*')
    if (-not $m.Success) { return @() }
    return @($m.Value.Split('.') | ForEach-Object { [int]$_ })
}
function Test-Newer([string]$a, [string]$b) {   # true when version a is newer than b
    $x = Get-Parts $a; $y = Get-Parts $b
    for ($i = 0; $i -lt [Math]::Max($x.Count, $y.Count); $i++) {
        $p = if ($i -lt $x.Count) { $x[$i] } else { 0 }
        $q = if ($i -lt $y.Count) { $y[$i] } else { 0 }
        if ($p -ne $q) { return $p -gt $q }
    }
    return $false
}
function Stop-WithMessage([string]$msg, [string]$color = 'Red') { Write-Host $msg -ForegroundColor $color; exit 1 }

Write-Host "Tools of Destruction Recompiled - update (this is version $ThisVersion)" -ForegroundColor Cyan
if (-not (Test-Path (Join-Path $root 'tod.exe'))) { Stop-WithMessage "tod.exe was not found in $root. Run Update.bat from the game folder." }

$url = ''
if ($Zip) {
    if (-not $Tag) { $Tag = 'v9.9' }
    Write-Host "Test mode: local file $Zip as version $Tag"
} else {
    try {
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        $rel = Invoke-RestMethod -Uri $api -Headers @{ 'User-Agent' = "tod-recomp-update/$ThisVersion"; 'Accept' = 'application/vnd.github+json' } -TimeoutSec 20
    } catch { Stop-WithMessage "Could not reach GitHub (offline?). You can also download the update from $page" }
    $Tag = [string]$rel.tag_name
    $asset = @($rel.assets | Where-Object { $_.name -like '*-win64.zip' }) | Select-Object -First 1
    if ($asset) { $url = [string]$asset.browser_download_url }
}
if (-not (Test-Newer $Tag $ThisVersion)) { Write-Host "You already have the latest version ($ThisVersion)." -ForegroundColor Green; exit 0 }
if (-not $Zip -and -not $url) { Stop-WithMessage "Version $Tag is out, but its download was not found. Get it from $page" }

$answer = Read-Host "Version $Tag is available. Download and install it now? (Y/N)"
if ($answer -notmatch '^[yY]') { Write-Host 'Nothing changed.'; exit 0 }

while (Get-Process tod -ErrorAction SilentlyContinue) {
    Read-Host 'The game is running. Close it, then press Enter'
}

$tmp = Join-Path $env:TEMP ("tod-recomp-update-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Path $tmp | Out-Null
try {
    if ($Zip) { $file = $Zip } else {
        $file = Join-Path $tmp 'update.zip'
        Write-Host "Downloading $url ..."
        $ProgressPreference = 'SilentlyContinue'
        Invoke-WebRequest -Uri $url -OutFile $file -UseBasicParsing -Headers @{ 'User-Agent' = "tod-recomp-update/$ThisVersion" }
    }
    $x = Join-Path $tmp 'x'
    Expand-Archive -LiteralPath $file -DestinationPath $x -Force
    $top = @(Get-ChildItem $x -Directory)
    $src = if ($top.Count -eq 1 -and -not (Test-Path (Join-Path $x 'tod.exe'))) { $top[0].FullName } else { $x }
    if (-not (Test-Path (Join-Path $src 'tod.exe')) -or -not (Test-Path (Join-Path $src 'Play.bat'))) {
        Stop-WithMessage 'The download does not look like a release of the port (no tod.exe / Play.bat). Nothing changed.'
    }
    $files = @(Get-ChildItem $src -Recurse -File -Force)
    $keep = @('tod.ini', 'gamedata', 'cache', 'game', 'elf', 'mods', 'log')   # first part of the path, never overwritten
    $n = 0
    foreach ($f in $files) {
        $rel = $f.FullName.Substring($src.Length + 1)
        if ($keep -contains ($rel.Split([char]92)[0]).ToLowerInvariant()) { continue }   # a release never holds these; never overwrite them anyway
        $dst = Join-Path $root $rel
        $dir = Split-Path $dst -Parent
        if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
        Copy-Item -LiteralPath $f.FullName -Destination $dst -Force
        $n++
    }
    $ini = Join-Path $root 'tod.ini'; $def = Join-Path $root 'tod.default.ini'
    if (-not (Test-Path $ini) -and (Test-Path $def)) { Copy-Item $def $ini }
    Write-Host "Updated to $Tag ($n files). Your settings, saves and shader cache were kept." -ForegroundColor Green
    Write-Host 'Start the game with Play.bat.'
} catch {
    Stop-WithMessage "The update failed: $($_.Exception.Message). Your game folder may be partly updated: download the release from $page and unzip it over the folder."
} finally {
    Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
}
