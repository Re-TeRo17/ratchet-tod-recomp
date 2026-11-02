<#
.SYNOPSIS
  Runs build\tod.exe and saves screenshots of its window at the given times.
    powershell -ExecutionPolicy Bypass -File tools\shot.ps1 -Times 20,40 -Tag s5
  Set TOD_INPUT_SCRIPT in the environment first for scripted button presses.
  Captures with PrintWindow (works on the D3D12 window, even in the background)
  in a DPI-aware process, so the image is the window's real size.
#>
param([int[]]$Times = @(20, 40), [string]$Tag = "run", [string]$Out = "out\shots")
Set-Location (Split-Path -Parent $PSScriptRoot)
New-Item -ItemType Directory -Force $Out | Out-Null
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
public static class TodShot {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint f);
  public struct RECT { public int L, T, R, B; }
}
'@
[void][TodShot]::SetProcessDPIAware()
$p = Start-Process build\tod.exe -ArgumentList "elf\EBOOT.ELF" -PassThru `
     -RedirectStandardOutput "$Out\$Tag.out.log" -RedirectStandardError "$Out\$Tag.err.log"
$t0 = Get-Date
foreach ($t in $Times) {
    while (((Get-Date) - $t0).TotalSeconds -lt $t) { Start-Sleep -Milliseconds 250 }
    $p.Refresh(); $h = $p.MainWindowHandle
    if ($h -eq [IntPtr]::Zero) { Write-Host "t=$t no window"; continue }
    $r = New-Object TodShot+RECT; [void][TodShot]::GetClientRect($h, [ref]$r)
    $bmp = New-Object System.Drawing.Bitmap ([Math]::Max(1, $r.R)), ([Math]::Max(1, $r.B))
    $g = [System.Drawing.Graphics]::FromImage($bmp); $hdc = $g.GetHdc()
    [void][TodShot]::PrintWindow($h, $hdc, 3)
    $g.ReleaseHdc($hdc); $g.Dispose()
    $f = Join-Path (Get-Location) "$Out\${Tag}_t$t.png"
    $bmp.Save($f); $bmp.Dispose()
    Write-Host "t=$t -> $f ($($r.R)x$($r.B))"
}
Stop-Process -Id $p.Id -Force
