# Starts tod.exe from the release folder with no console window; stdout+stderr go to log\tod.log.
# Not for double-clicking: use Play.bat. Extra arguments are passed to tod.exe.
$ErrorActionPreference = 'Stop'
function Fail($msg) {
    Write-Host ''
    Write-Host $msg -ForegroundColor Red
    Read-Host 'Press Enter to close'
    exit 1
}
try {
    $root = (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)).TrimEnd('\')
    $exe = Join-Path $root 'tod.exe'
    $logDir = Join-Path $root 'log'
    New-Item -ItemType Directory -Force -Path $logDir | Out-Null
    $log = Join-Path $logDir 'tod.log'
    $prev = Join-Path $logDir 'tod.previous.log'
    if (Test-Path -LiteralPath $log) { Move-Item -LiteralPath $log -Destination $prev -Force -ErrorAction SilentlyContinue }
    if (-not (Test-Path -LiteralPath $exe)) { Fail "tod.exe not found in $root" }
    $errLog = Join-Path $logDir 'tod.stderr.tmp'
    # 1.01: Windows PowerShell 5.1 refuses an empty -ArgumentList, so it is only passed when there are arguments
    # (1.0 failed here on every plain double-click of Play.bat: no game, no log).
    $sp = @{ FilePath = $exe; WorkingDirectory = $root; WindowStyle = 'Normal'; PassThru = $true
             RedirectStandardOutput = $log; RedirectStandardError = $errLog }
    if ($args.Count -gt 0) { $sp.ArgumentList = $args }
    $started = Get-Date
    $p = Start-Process @sp
    $null = $p.Handle
    $p.WaitForExit()
} catch {
    Fail ("Could not start the game: " + $_.Exception.Message)
}
# one log file: stdout then stderr
try {
    Add-Content -LiteralPath $log -Value "`r`n==== stderr ====`r`n" -ErrorAction Stop
    Get-Content -LiteralPath $errLog -Raw | Add-Content -LiteralPath $log
    Remove-Item -LiteralPath $errLog -ErrorAction SilentlyContinue
} catch { }
# A crash or an error right after the start: say so instead of closing silently.
if ($p.ExitCode -ne 0 -and ((Get-Date) - $started).TotalSeconds -lt 30) {
    Fail ("The game closed with an error (exit code $($p.ExitCode)). The log is in $log - please attach it to your report.")
}
exit $p.ExitCode
