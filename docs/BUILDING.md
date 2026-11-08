# Building from source

<a id="top"></a>

[Back to the README](../README.md)

This repository contains the port's own code (runtime glue, installer, settings page, tools) and the
build scripts. It does **not** contain any code or data generated from the game: the game's PowerPC
(PPU) and SPU code is translated from **your own** `EBOOT.ELF` on your machine, into the git-ignored
folders `recompiled\` and `spu\`. You need your own copy of the disc (European release, BCES00052,
v1.00) to build a working `tod.exe`.

The released `tod.exe` is built the same way (plus the optional step 7 below).

## 1. Tools

| | |
|---|---|
| Windows 10/11 x64 | The build is Windows-only (Direct3D 12). |
| Visual Studio 2022 or newer, or its Build Tools | "Desktop development with C++" workload with the Windows SDK (provides the MSVC headers, `lib.exe`, `vcvars64.bat`). |
| [LLVM](https://releases.llvm.org) with `clang-cl` | The lifted code is compiled with clang-cl (MSVC cl.exe is not supported for it). In `PATH` or `C:\Program Files\LLVM`. |
| [CMake](https://cmake.org) 3.20+ and [Ninja](https://ninja-build.org) | `winget install Kitware.CMake Ninja-build.Ninja` |
| Python 3.11+ | For the lifter and the tools (a virtual environment is created below). |
| [vcpkg](https://vcpkg.io) | `vcpkg install zlib:x64-windows sdl2:x64-windows`. Expected in `C:\vcpkg`, or set `VCPKG_ROOT`. |
| Git | |

Plan for about 16 GB of RAM (the generated source files are 30-50 MB each) and about 5 GB of disk for
`recompiled\`, `spu\` and `build\`. From a clean checkout the whole procedure (steps 3-6) took about
16 minutes on a Ryzen 5 3600 (6 cores): PPU lift ~7 min, SPU lift <1 min, compile and link ~8 min.

## 2. Get the source

`ps3recomp` (the runtime this port is built on) is a git submodule, a fork of
[soyjxck/ps3recomp](https://github.com/soyjxck/ps3recomp) that carries this port's runtime changes:

```powershell
git clone --recurse-submodules https://github.com/Re-TeRo17/ratchet-tod-recomp.git
cd ratchet-tod-recomp
```

(If you already cloned without `--recurse-submodules`: `git submodule update --init`.)

## 3. Python environment

```powershell
python -m venv .venv
.venv\Scripts\pip install -r ps3recomp\requirements.txt
```

## 4. The game executable (`elf\EBOOT.ELF`)

The lifter works on the decrypted ELF of the game's `EBOOT.BIN`. Take it from your own **decrypted**
disc dump (the folder that contains `PS3_GAME`, or a decrypted `.iso`: see
[Which game files work](../README.md#which-game-files-work)). Either

- use the installer of the release (`tod_setup.exe --install --base . <iso or disc folder>`), or the
  built `tod.exe --install <iso or disc folder>` once you have one; both also copy the disc into
  `game\disc`, which you need to play; or
- decrypt the file yourself: `.venv\Scripts\python tools\self2elf.py <path to PS3_GAME\USRDIR\EBOOT.BIN> elf\EBOOT.ELF`.

The result must be the ELF with SHA-256 `cd2c37351e840fb3...` (the installer checks this for you).
`elf\` is git-ignored.

## 5. Generate the code from the ELF

```powershell
powershell -ExecutionPolicy Bypass -File tools\lift_ppu.ps1   # PPU: elf\EBOOT.ELF -> recompiled\ (minutes, ~600 MB of C++)
powershell -ExecutionPolicy Bypass -File tools\lift_spu.ps1   # SPU: policy module, 21 job modules, 2 raw collision programs -> spu\
```

`lift_ppu.ps1` runs `ps3recomp\tools\ppu_loader.py` and `ppu_lifter.py` (function discovery, then
translation to C++) and computes `--code-end` from the ELF's section headers. `lift_spu.ps1` extracts
the SPU programs embedded in the ELF, lifts them with `ps3recomp\tools\spu_lifter.py` and writes the
registries the build links. Both are re-runnable (`lift_spu.ps1 -Force` redoes everything).

## 6. Configure and build

```powershell
tools\build_win.bat            # = configure + build; output: build\tod.exe, SDL2.dll, z.dll, tod.ini
```

`tools\build_win.bat` finds Visual Studio (`vswhere`), clang-cl, CMake, Ninja and vcpkg, then runs

```
cmake -S . -B build -G Ninja -DRECOMP_DIR=<repo>/recompiled -DCMAKE_BUILD_TYPE=Release ^
      -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl ^
      -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build
```

Useful options: `-DSPU_DIR=<dir>` (where the lifted SPU images are, default `spu\`),
`-DPS3RECOMP_DIR=<dir>` (use another ps3recomp checkout), `-DTOD_SYMBOLS=OFF` (no debug info; much
smaller `tod.exe`), `-DTOD_CONSOLE=OFF` (windowed program, no console), `-DTOD_X86_LEVEL=` /
`-DTOD_SPU_X86_LEVEL=` (the default `x86-64-v3` needs AVX2/FMA; `x86-64-v2` is the SSE4.1 level for
the SPU code). `build_win.bat build` repeats only the compile step. Build output paths are not
embedded in the executable (`-fmacro-prefix-map`, `/PDBALTPATH`).

## 7. Optional: dynamic SPU overlays (speed)

Some of the game's SPU code is loaded at run time and cannot be found in the ELF. Without extra
work those regions run in ps3recomp's SPU interpreter (correct, but slower). The release build has
them lifted too, from regions captured while playing:

```powershell
$env:SPU_DUMP_UNLIFTED = "spu_dumps"        # then play through the game's levels once; the runtime saves the regions it had to interpret
build\tod.exe
.venv\Scripts\python -I tools\lift_dynovl.py --dumps spu_dumps --out spu      # lift them; writes spu\dynovl_register.c
tools\build_win.bat                          # re-run CMake: the new images are picked up
```

The release also uses two **native fast paths** for the game's heaviest SPU jobs: skeletal animation
(`SPU_HLE_WID0`, on by default; `=0` turns it off) and the collision tickets (`SPU_COLL_SYNC`, on by default;
`=0` turns it off). They need source files that are a mechanical translation of SPU functions of the game
(`ps3recomp\runtime\spu\anim_gen_*.inc`, `spu_coll_l2_gen_*.inc`, `spu_coll_l2_img.inc`, `spu_coll_sync_regs.inc`) and
small hooks inserted into the lifted jobs. All of that is game-derived, so it is not in the repository: without it
CMake switches the collision path off by itself (message `TOD_COLL_SYNC: generated collision tables not present`)
and the animation code refuses every command, so a build from this source runs the game's own lifted animation and
collision SPU programs instead (same result, slower: about 2.4 ms per frame in animation and about 1.4 ms in collision
in dense scenes such as Ardolis).

The release's lifted job modules are also post-processed with `tools\rz109_apply.py <in spu_recomp.c> <out spu_recomp.c>`
(float-heavy loops and functions of the SPURS job modules run with the host's round-toward-zero float operations;
`RZ109=0` turns it off at run time, `--check` compares every operation with the exact path). The patched modules are
generated code, so they are not published either; without the step the exact path is used.

`spu_dumps\` is git-ignored (it is game code). Other optional post-lift helpers, only for profiling and
debugging: `tools\patch_ppu_timeline.py` (TOD_TIMELINE hooks), `tools\patch_ppu_hints.py` (PPU pause
hints), `tools\fntrace_patch.py` (function tracing).

## 8. Run

Install the game next to the built executable (`build\`), as the release does:

```powershell
cd build
.\tod.exe --install ..\path\to\disc.iso      # or the disc folder; --check <file> to see what it would do
.\tod.exe
```

or copy `build\tod.exe`, `SDL2.dll`, `z.dll`, `tod.ini` and the files of `release\` into a folder
and use `Install.bat` / `Play.bat` as in the release.

## What is where

```
src\                  the port: main.cpp (loader, frame clock), install\ (disc installer), settings.cpp (tod.ini),
                      ui\ (F1 settings page, Dear ImGui), patches\ (button prompts, SIXAXIS), input\, kernel\, stubs\
tools\                lift_ppu.ps1, lift_spu.ps1, lift_dynovl.py, tod_spu.py, self2elf.py, build_win.bat, build_setup.bat (tod_setup.exe), ...
tools\analysis\       small read-only analysis helpers used while reverse engineering (need your own extracted data)
release\              Install.bat, Play.bat and the PowerShell scripts shipped in the release zip
docs\                 developer notes (partly in Italian): SPU inventory, raw SPU collision programs, frame rate, camera, textures...
third_party\imgui\    Dear ImGui 1.92.9b (MIT)
ps3recomp\            git submodule: the recompiler and runtime (fork of soyjxck/ps3recomp, MIT)
```

<sub><a href="#top">back to top</a></sub>
