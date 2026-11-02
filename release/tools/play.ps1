# Starts tod.exe from the release folder with no console window; stdout+stderr go to log\tod.log.
# Not for double-clicking: use Play.bat. Extra arguments are passed to tod.exe.
$ErrorActionPreference = 'Stop'
$root = (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)).TrimEnd('\')
$exe = Join-Path $root 'tod.exe'
$logDir = Join-Path $root 'log'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$log = Join-Path $logDir 'tod.log'
$prev = Join-Path $logDir 'tod.previous.log'
if (Test-Path -LiteralPath $log) { Move-Item -LiteralPath $log -Destination $prev -Force -ErrorAction SilentlyContinue }
if (-not (Test-Path -LiteralPath $exe)) { Write-Host "tod.exe not found in $root" -ForegroundColor Red; Read-Host 'Enter to close'; exit 1 }
$errLog = Join-Path $logDir 'tod.stderr.tmp'
$p = Start-Process -FilePath $exe -WorkingDirectory $root -ArgumentList $args -WindowStyle Normal -PassThru `
    -RedirectStandardOutput $log -RedirectStandardError $errLog
$null = $p.Handle
$p.WaitForExit()
# one log file: stdout then stderr
try {
    Add-Content -LiteralPath $log -Value "`r`n==== stderr ====`r`n" -ErrorAction Stop
    Get-Content -LiteralPath $errLog -Raw | Add-Content -LiteralPath $log
    Remove-Item -LiteralPath $errLog -ErrorAction SilentlyContinue
} catch { }
exit $p.ExitCode
