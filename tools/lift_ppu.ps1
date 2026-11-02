<#
.SYNOPSIS
  Lifts elf\EBOOT.ELF to C++ with ps3recomp's ppu_lifter.py (output: recompiled\).

.DESCRIPTION
  Same recipe as dod3-recomp\docs\BUILDING.md ("Lifting the PPU code"), minus the
  game-specific --hook list:
    ppu_loader.py   (once; writes out\v100\EBOOT.{functions,imports,image,loader}.json)
    ppu_lifter.py --functions ... --hle-stubs ... --code-end ... --nonvolatile-locals

  --code-end is the end of the last executable section of the ELF, computed by
  tools\elf_code_end.py from the section headers (SHF_EXECINSTR). For this EBOOT:
  .text 0x10280..0x8393A8 plus the import-stub sections ..0x83A28C -> 0x83A28C.
  (It stops .rodata in the R-X segment, 0x83A28C..0x83C3C8, from being promoted
  to functions; the import stubs must stay inside it, see ppu_lifter.py
  _outside_code.)  Use -CodeEnd to override.

  Does not need the compilers; needs the .venv and a ps3recomp checkout.
#>
param(
    [string]$Ps3recompDir = "",        # default: .\ps3recomp, else ..\ref\dod3-recomp\ps3recomp
    [string]$Elf     = "elf\EBOOT.ELF",
    [string]$OutDir  = "out\v100",     # ppu_loader.py output
    [string]$Recomp  = "recompiled",   # lifter output (CMake RECOMP_DIR)
    [string]$CodeEnd = "",             # hex; default: computed from the ELF
    [switch]$SkipLoader,               # reuse the JSONs already in $OutDir
    [int]$Jobs = 0                     # 0 = lifter default (CPU count)
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$py = Join-Path $root ".venv\Scripts\python.exe"
if (-not (Test-Path $py)) { throw "no .venv: python -m venv .venv; .venv\Scripts\pip install -r <ps3recomp>\tools\requirements.txt" }

if (-not $Ps3recompDir) {
    foreach ($c in @("ps3recomp", "..\ref\dod3-recomp\ps3recomp")) {
        if (Test-Path (Join-Path $c "tools\ppu_lifter.py")) { $Ps3recompDir = $c; break }
    }
}
if (-not $Ps3recompDir -or -not (Test-Path (Join-Path $Ps3recompDir "tools\ppu_lifter.py"))) {
    throw "ps3recomp not found: add the submodule or pass -Ps3recompDir"
}
Write-Host "ps3recomp: $Ps3recompDir"
if (-not (Test-Path $Elf)) { throw "missing $Elf (decrypt the EBOOT with tools\self2elf.py)" }

if (-not $CodeEnd) { $CodeEnd = (& $py tools\elf_code_end.py $Elf).Trim() }
Write-Host "code-end:  $CodeEnd"

$stem = [IO.Path]::GetFileNameWithoutExtension($Elf)      # EBOOT
if (-not $SkipLoader -or -not (Test-Path "$OutDir\$stem.functions.json")) {
    & $py "$Ps3recompDir\tools\ppu_loader.py" $Elf -o "$OutDir\"
    if ($LASTEXITCODE -ne 0) { throw "ppu_loader.py failed" }
}

$liftArgs = @(
    "$Ps3recompDir\tools\ppu_lifter.py", $Elf,
    "--functions", "$OutDir\$stem.functions.json",
    "--hle-stubs", "$OutDir\$stem.imports.json",
    "--code-end",  $CodeEnd,
    "--nonvolatile-locals",
    # Lua 5.1 setjmp/longjmp: luaD_rawrunprotected + longjmp are native (src/stubs/lua_eh.cpp)
    "--hook", "0x671A28", "--hook", "0x8268B4",
    "-o", "$Recomp\"
)
if ($Jobs -gt 0) { $liftArgs += @("--jobs", "$Jobs") }
Write-Host "lifting -> $Recomp (takes minutes, writes hundreds of MB)"
& $py @liftArgs
if ($LASTEXITCODE -ne 0) { throw "ppu_lifter.py failed" }
Write-Host "done. Next: tools\build_win.bat"

