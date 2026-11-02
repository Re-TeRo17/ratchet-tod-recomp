# Extract and lift every SPU program the port runs, into spu\ (git-ignored):
#   - the Wws_Job SPURS policy module (the job manager every WWS workload runs)
#   - the 21 WWS job modules (C0DEC0DE) embedded in the EBOOT's data segment
#   - the 2 raw-SPU collision programs (igCollBroadSpu, igcollspu)
# then generate the registries the build compiles (spu\spu_workloads.c,
# spu\jobmods_register.c, spu\spu_raw_workloads.c).  PowerShell analogue of dod3's tools\lift_spu.sh.
#
# Not lifted on purpose: the Edge zlib SPU task (src\kernel\edgezlib_native.cpp runs it natively).
#
# Usage (from tod-recomp):  powershell -File tools\lift_spu.ps1 [-Force]
#   -Force   wipe spu\ lifts and redo everything (default keeps existing lifts)
#   -NoPromote  skip step 5 (register-promoted twins of every lifted function, see below)
param([switch]$Force, [switch]$NoPromote)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$env:PYTHONIOENCODING = "utf-8"
$py = Join-Path $root ".venv\Scripts\python.exe"
if (-not (Test-Path $py)) { throw "lift_spu: no $py (docs\BUILDING.md)" }
$elf = if ($env:TOD_ELF) { $env:TOD_ELF } else { "elf\EBOOT.ELF" }
if (-not (Test-Path $elf)) { throw "lift_spu: no $elf" }
$T = "ps3recomp\tools"

function Run-Py {   # run a python tool, fail on non-zero exit (stderr is progress output, not an error)
    $old = $ErrorActionPreference; $ErrorActionPreference = "Continue"
    & $py -I @args 2>&1 | ForEach-Object { "$_" }
    $rc = $LASTEXITCODE; $ErrorActionPreference = $old
    if ($rc -ne 0) { throw "lift_spu: $($args[0]) failed ($rc)" }
}

if ($Force -and (Test-Path spu)) {
    Get-ChildItem spu -Directory | Where-Object { $_.Name -ne "images" } | Remove-Item -Recurse -Force
    Remove-Item spu\*.c, spu\*.json -Force -ErrorAction SilentlyContinue
}
New-Item -ItemType Directory -Force spu | Out-Null

# 1. policy module + job modules out of the EBOOT
Run-Py tools\tod_spu.py extract --elf $elf --out spu

# 2. the policy module, registered by the fingerprint cellSpurs computes (image id 2)
Run-Py tools\tod_spu.py lift-pm --out spu

# 2b. the two raw-SPU collision programs (igCollBroadSpu, igcollspu): lifted to
#     spu\<name>\ and registered by spu\spu_raw_workloads.c (docs\RAW_SPU.md)
Run-Py tools\tod_spu.py lift-raw --out spu

# 3. the job modules: all lifted at LS 0x4000 (the job code buffer), selected at run
#    time by their 16-byte header signature (jobmods_register.c)
if ($Force -or -not (Test-Path spu\jobmods_manifest.json)) {
    Run-Py "$T\lift_jobmods.py" --jobmods spu\jobmods --out spu --register spu\jobmods_register.c --symbol-prefix tod
}

# 4. the lifted sources use bare #include "spu_context.h" (CMake adds runtime\spu)
Run-Py tools\tod_spu.py debase --out spu
# 5. register-promoted twins (<fn>_p, SPU registers in C locals; selected at run time with SPU_PROMOTE=1,
#    equivalence check SPU_PROMOTE_CHECK=<n>). Idempotent; plain functions are untouched.
if (-not $NoPromote) { Run-Py "$T\promote_spu_c.py" spu }
Write-Host "lift_spu: done"
