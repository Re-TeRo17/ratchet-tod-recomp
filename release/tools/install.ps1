<#
  Ratchet & Clank: Tools of Destruction Recompiled -- installer (v1.0)

  Install.bat starts this. It asks for your own DECRYPTED disc (a .iso or the folder that
  holds PS3_GAME) and a game language, then calls tod_setup.exe, which checks the disc
  (Title ID BCES00052, version 01.00, SHA-256 of EBOOT.BIN), copies it into game\disc and
  makes elf\EBOOT.ELF from its EBOOT.BIN. The language goes into tod.ini (PS3_LANG).

  Without arguments: a small window. With -Source: no window (for scripts and tests).

    install.ps1 [-Source <.iso or folder>] [-Lang <name>] [-Base <folder>] [-NoLaunch]

  -Lang is a PS3_LANG value: english english_gb french german spanish italian dutch
  portuguese swedish norwegian danish finnish korean chinese_t japanese
#>
param(
    [string]$Source = '',
    [string]$Lang = '',
    [string]$Base = '',
    [switch]$NoLaunch
)
$ErrorActionPreference = 'Stop'

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if ($Base) { $root = (Resolve-Path -LiteralPath $Base).Path } else { $root = Split-Path -Parent $here }
$root = $root.TrimEnd('\')
$setup = Join-Path $root 'tod_setup.exe'
$ini = Join-Path $root 'tod.ini'
$title = 'Ratchet & Clank: Tools of Destruction Recompiled - Setup'

# Language list: what the EU disc (BCES00052) carries (En Ja Fr De Es It Nl Pt Sv No Da Fi Zh Ko).
$languages = [ordered]@{
    'English (US)'          = 'english'
    'English (UK)'          = 'english_gb'
    'Francais'              = 'french'
    'Deutsch'               = 'german'
    'Espanol'               = 'spanish'
    'Italiano'              = 'italian'
    'Nederlands'            = 'dutch'
    'Portugues'             = 'portuguese'
    'Svenska'               = 'swedish'
    'Norsk'                 = 'norwegian'
    'Dansk'                 = 'danish'
    'Suomi'                 = 'finnish'
    'Korean'                = 'korean'
    'Chinese (Traditional)' = 'chinese_t'
    'Japanese'              = 'japanese'
}

function Get-IniLang {
    if (Test-Path -LiteralPath $ini) {
        foreach ($l in Get-Content -LiteralPath $ini) {
            if ($l -match '^\s*PS3_LANG\s*=\s*(\S+)') { return $Matches[1].ToLower() }
        }
    }
    return 'english'
}

# Writes PS3_LANG into tod.ini (replaces the active line, else adds one); the rest is kept.
function Set-IniLang([string]$value) {
    $lines = @()
    if (Test-Path -LiteralPath $ini) { $lines = @(Get-Content -LiteralPath $ini) }
    $done = $false
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match '^\s*PS3_LANG\s*=') { $lines[$i] = "PS3_LANG = $value"; $done = $true; break }
    }
    if (-not $done) { $lines += "PS3_LANG = $value" }
    [IO.File]::WriteAllLines($ini, [string[]]$lines, (New-Object Text.UTF8Encoding($false)))
}

function Test-Installed {
    if (-not (Test-Path -LiteralPath $setup)) { return $false }
    & $setup --status --base $root *> $null
    return ($LASTEXITCODE -eq 0)
}

# Asks tod_setup what the file is. Returns @{ Ok=bool; Text=string }.
function Test-Source([string]$path) {
    $path = $path.Trim().Trim('"').TrimEnd('\')
    if (-not $path) { return @{ Ok = $false; Text = '' } }
    $out = (& $setup --check $path 2>&1 | Out-String).Trim()
    return @{ Ok = ($LASTEXITCODE -eq 0); Text = $out }
}

function Test-VcRuntime {
    $sys = Join-Path $env:WINDIR 'System32'
    return (Test-Path (Join-Path $sys 'msvcp140.dll')) -and (Test-Path (Join-Path $sys 'vcruntime140_1.dll'))
}

# Starts tod_setup --install; output goes to a file that is polled. Returns @{ Proc; Log }.
function Start-Install([string]$src) {
    $src = $src.Trim().Trim('"').TrimEnd('\')
    $log = Join-Path $env:TEMP ("tod_setup_{0}.log" -f $PID)
    Remove-Item -LiteralPath $log -ErrorAction SilentlyContinue
    $arg = '--install --machine --base "{0}" "{1}"' -f $root, $src
    $p = Start-Process -FilePath $setup -ArgumentList $arg -RedirectStandardOutput $log -WindowStyle Hidden -PassThru
    $null = $p.Handle
    return @{ Proc = $p; Log = $log }
}

# Last status of a running install: @{ Pct; What; Error; Done }
function Get-InstallState($job) {
    $r = @{ Pct = 0; What = 'starting'; Error = ''; Done = $false }
    try {
        $fs = New-Object IO.FileStream($job.Log, 'Open', 'Read', 'ReadWrite')
        $sr = New-Object IO.StreamReader($fs)
        $text = $sr.ReadToEnd(); $sr.Close(); $fs.Close()
    } catch { return $r }
    foreach ($l in ($text -split "`r?`n")) {
        if ($l -match '^PROGRESS (\d+) (.*)$') { $r.Pct = [int]$Matches[1]; $r.What = $Matches[2] }
        elseif ($l -match '^ERROR (.*)$') { $r.Error = $Matches[1] }
        elseif ($l -eq 'DONE') { $r.Done = $true }
    }
    return $r
}

function Install-Finish([string]$langValue) {
    Set-IniLang $langValue
}

# ------------------------------------------------------------------------------------------
if (-not (Test-Path -LiteralPath $setup)) {
    Write-Host "tod_setup.exe was not found next to Install.bat ($setup). Unzip the whole release first." -ForegroundColor Red
    exit 1
}

# ---- no window: -Source given ---------------------------------------------------------------
if ($Source) {
    if ($Lang -and -not ($languages.Values -contains $Lang.ToLower())) {
        Write-Host "Unknown language '$Lang'. Use one of: $($languages.Values -join ' ')" -ForegroundColor Red
        exit 2
    }
    $c = Test-Source $Source
    if (-not $c.Ok) { Write-Host "Cannot install from that: $($c.Text)" -ForegroundColor Red; exit 1 }
    Write-Host $c.Text
    $job = Start-Install $Source
    $last = -1; $lastWhat = ''
    while (-not $job.Proc.HasExited) {
        $s = Get-InstallState $job
        if ($s.Pct -ne $last -or $s.What -ne $lastWhat) {
            if (($s.Pct % 5 -eq 0) -or ($s.What -ne $lastWhat)) { Write-Host ("  {0,3}%  {1}" -f $s.Pct, $s.What) }
            $last = $s.Pct; $lastWhat = $s.What
        }
        Start-Sleep -Milliseconds 400
    }
    $s = Get-InstallState $job
    if ($job.Proc.ExitCode -ne 0 -or -not $s.Done) {
        Write-Host "Install failed: $($s.Error)" -ForegroundColor Red
        exit 1
    }
    if ($Lang) { Install-Finish $Lang.ToLower() }
    Write-Host 'Installed. Start the game with Play.bat.' -ForegroundColor Green
    if (-not (Test-VcRuntime)) {
        Write-Host 'Warning: the Microsoft Visual C++ Redistributable (x64) does not seem to be installed: https://aka.ms/vs/17/release/vc_redist.x64.exe' -ForegroundColor Yellow
    }
    exit 0
}

# ---- the window ----------------------------------------------------------------------------
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[Windows.Forms.Application]::EnableVisualStyles()

$form = New-Object Windows.Forms.Form
$form.Text = $title
$form.StartPosition = 'CenterScreen'
$form.FormBorderStyle = 'FixedDialog'
$form.MaximizeBox = $false
$form.ClientSize = New-Object Drawing.Size(640, 430)
$form.Font = New-Object Drawing.Font('Segoe UI', 9)

function New-Label($text, $x, $y, $w, $h) {
    $l = New-Object Windows.Forms.Label
    $l.UseMnemonic = $false
    $l.Text = $text; $l.Location = New-Object Drawing.Point($x, $y); $l.Size = New-Object Drawing.Size($w, $h)
    $form.Controls.Add($l); return $l
}

$h = New-Label 'Ratchet & Clank: Tools of Destruction Recompiled  1.0' 16 12 608 24
$h.Font = New-Object Drawing.Font('Segoe UI', 12, [Drawing.FontStyle]::Bold)
$null = New-Label ("Nothing of the game is included: this copies YOUR disc into this folder.`r`n" +
    "You need the European release (BCES00052, v1.00) as a DECRYPTED .iso, or the folder that holds PS3_GAME.") 16 42 608 40

$null = New-Label 'Your disc (decrypted .iso or folder):' 16 92 400 18
$tbSrc = New-Object Windows.Forms.TextBox
$tbSrc.Location = New-Object Drawing.Point(16, 112); $tbSrc.Size = New-Object Drawing.Size(430, 24)
$form.Controls.Add($tbSrc)
$btIso = New-Object Windows.Forms.Button
$btIso.Text = '.iso...'; $btIso.Location = New-Object Drawing.Point(454, 110); $btIso.Size = New-Object Drawing.Size(80, 27)
$form.Controls.Add($btIso)
$btDir = New-Object Windows.Forms.Button
$btDir.Text = 'Folder...'; $btDir.Location = New-Object Drawing.Point(542, 110); $btDir.Size = New-Object Drawing.Size(82, 27)
$form.Controls.Add($btDir)
$lbCheck = New-Label '' 16 142 608 58
$lbCheck.ForeColor = [Drawing.Color]::DimGray

$null = New-Label 'Game language (text, voices, subtitles; changeable later in the F1 menu):' 16 208 608 18
$cbLang = New-Object Windows.Forms.ComboBox
$cbLang.DropDownStyle = 'DropDownList'
$cbLang.Location = New-Object Drawing.Point(16, 228); $cbLang.Size = New-Object Drawing.Size(250, 24)
foreach ($k in $languages.Keys) { [void]$cbLang.Items.Add($k) }
$cur = Get-IniLang
$idx = 0; $i = 0
foreach ($v in $languages.Values) { if ($v -eq $cur) { $idx = $i }; $i++ }
$cbLang.SelectedIndex = $idx
$form.Controls.Add($cbLang)

$null = New-Label ("Installs into: $root`r`nNeeds about 23 GB of free disk space on that drive.") 16 266 608 36
$lbState = New-Label '' 16 306 608 20
$pb = New-Object Windows.Forms.ProgressBar
$pb.Location = New-Object Drawing.Point(16, 328); $pb.Size = New-Object Drawing.Size(608, 22); $pb.Minimum = 0; $pb.Maximum = 100
$form.Controls.Add($pb)

$btInstall = New-Object Windows.Forms.Button
$btInstall.Text = 'Install'; $btInstall.Location = New-Object Drawing.Point(16, 372); $btInstall.Size = New-Object Drawing.Size(120, 34)
$btInstall.Enabled = $false
$form.Controls.Add($btInstall)
$btPlay = New-Object Windows.Forms.Button
$btPlay.Text = 'Play'; $btPlay.Location = New-Object Drawing.Point(144, 372); $btPlay.Size = New-Object Drawing.Size(100, 34)
$form.Controls.Add($btPlay)
$btClose = New-Object Windows.Forms.Button
$btClose.Text = 'Close'; $btClose.Location = New-Object Drawing.Point(524, 372); $btClose.Size = New-Object Drawing.Size(100, 34)
$form.Controls.Add($btClose)

$script:sourceOk = $false
$script:job = $null
$script:installed = Test-Installed

function Update-Buttons {
    $busy = ($script:job -ne $null)
    $btPlay.Enabled = $script:installed -and -not $busy
    $btIso.Enabled = -not $busy; $btDir.Enabled = -not $busy; $tbSrc.Enabled = -not $busy; $cbLang.Enabled = -not $busy
    $btClose.Enabled = -not $busy
    if ($script:sourceOk) { $btInstall.Text = $(if ($script:installed) { 'Reinstall' } else { 'Install' }); $btInstall.Enabled = -not $busy }
    elseif ($script:installed) { $btInstall.Text = 'Save language'; $btInstall.Enabled = -not $busy }
    else { $btInstall.Text = 'Install'; $btInstall.Enabled = $false }
}

function Check-Source {
    $c = Test-Source $tbSrc.Text
    $script:sourceOk = $c.Ok
    if ($tbSrc.Text.Trim() -eq '') {
        $lbCheck.Text = $(if ($script:installed) { 'The game is installed. Pick a language and "Save language", or choose a disc to reinstall.' } else { '' })
        $lbCheck.ForeColor = [Drawing.Color]::DimGray
    } elseif ($c.Ok) {
        $lbCheck.Text = $c.Text; $lbCheck.ForeColor = [Drawing.Color]::DarkGreen
    } else {
        $lbCheck.Text = $c.Text; $lbCheck.ForeColor = [Drawing.Color]::Firebrick
    }
    Update-Buttons
}

$btIso.Add_Click({
    $d = New-Object Windows.Forms.OpenFileDialog
    $d.Filter = 'Decrypted PS3 disc image (*.iso)|*.iso|All files (*.*)|*.*'
    $d.Title = 'Choose the decrypted .iso of BCES00052'
    if ($d.ShowDialog() -eq 'OK') { $tbSrc.Text = $d.FileName; Check-Source }
})
$btDir.Add_Click({
    $d = New-Object Windows.Forms.FolderBrowserDialog
    $d.Description = 'Choose the folder that contains PS3_GAME'
    if ($d.ShowDialog() -eq 'OK') { $tbSrc.Text = $d.SelectedPath; Check-Source }
})
$tbSrc.Add_Leave({ Check-Source })
$tbSrc.Add_KeyDown({ if ($_.KeyCode -eq 'Enter') { Check-Source; $_.SuppressKeyPress = $true } })

$timer = New-Object Windows.Forms.Timer
$timer.Interval = 300
$timer.Add_Tick({
    if ($script:job -eq $null) { return }
    $s = Get-InstallState $script:job
    $pb.Value = [Math]::Min(100, [Math]::Max(0, $s.Pct))
    $lbState.Text = ('{0}  {1}%' -f $s.What, $s.Pct)
    if ($script:job.Proc.HasExited) {
        $timer.Stop()
        $ok = ($script:job.Proc.ExitCode -eq 0 -and $s.Done)
        $script:job = $null
        if ($ok) {
            Install-Finish $languages[[string]$cbLang.SelectedItem]
            $script:installed = $true
            $lbState.Text = 'Installed.'
            $pb.Value = 100
            [void][Windows.Forms.MessageBox]::Show("Installed. Press Play to start the game.`r`n(F1 in game opens the settings.)", $title)
        } else {
            $lbState.Text = 'Install failed.'
            [void][Windows.Forms.MessageBox]::Show("Install failed:`r`n$($s.Error)", $title, 'OK', 'Error')
        }
        Update-Buttons
    }
})

$btInstall.Add_Click({
    $langValue = $languages[[string]$cbLang.SelectedItem]
    if (-not $script:sourceOk) {            # already installed: only the language
        Set-IniLang $langValue
        $lbState.Text = "Language saved: $($cbLang.SelectedItem)."
        return
    }
    if ($script:installed) {
        $a = [Windows.Forms.MessageBox]::Show('Replace the installed game files with this disc?', $title, 'YesNo', 'Question')
        if ($a -ne 'Yes') { return }
    }
    $pb.Value = 0
    $script:job = Start-Install $tbSrc.Text
    $timer.Start()
    Update-Buttons
})
$btPlay.Add_Click({
    $play = Join-Path $root 'Play.bat'
    Start-Process -FilePath $play -WorkingDirectory $root
    $form.Close()
})
$btClose.Add_Click({ $form.Close() })
$form.Add_FormClosing({
    if ($script:job -ne $null) { $_.Cancel = $true }    # not while copying
})

if (-not (Test-VcRuntime)) {
    $lbState.Text = 'Warning: install the Microsoft Visual C++ Redistributable (x64): https://aka.ms/vs/17/release/vc_redist.x64.exe'
    $lbState.ForeColor = [Drawing.Color]::DarkOrange
}
Check-Source
[void]$form.ShowDialog()
