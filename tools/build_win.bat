@echo off
rem Windows build: clang-cl (LLVM) inside a Visual Studio developer environment.
rem Usage:  tools\build_win.bat [configure|build|all] [ninja args]   (default: all)
rem Builds the lift in recompiled\ (tools\lift_ppu.ps1) into build\.
rem
rem Needs Visual Studio 2022 or newer (the Build Tools are enough), LLVM with
rem clang-cl (on PATH, or in C:\Program Files\LLVM), CMake and Ninja, and
rem vcpkg with zlib:x64-windows and sdl2:x64-windows (VCPKG_ROOT, or C:\vcpkg).
rem Optional: PS3RECOMP_DIR=<checkout> (default: the ps3recomp submodule).
setlocal
set ACTION=%1
if "%ACTION%"=="" set ACTION=all
set BDIR=build
set RECOMP=recompiled
if not defined VCPKG_ROOT set VCPKG_ROOT=C:\vcpkg

set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
set VCVARS=
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -products * -latest -property installationPath`) do set VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VCVARS%" set VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VCVARS%" echo build_win: Visual Studio was not found (vcvars64.bat) & exit /b 1
rem vcvars64 points VCPKG_ROOT at the copy bundled with Visual Studio: keep ours.
set TOD_VCPKG=%VCPKG_ROOT%
call "%VCVARS%" >nul
set VCPKG_ROOT=%TOD_VCPKG%
rem Ninja: on PATH, else where winget installs it (its Links folder is not
rem always on PATH).
for %%c in (ninja.exe) do set NINJA=%%~$PATH:c
if not defined NINJA for /f "delims=" %%i in ('dir /s /b "%LOCALAPPDATA%\Microsoft\WinGet\Packages\ninja.exe" 2^>nul') do set NINJA=%%i
if not exist "%NINJA%" echo build_win: ninja was not found & exit /b 1
for %%c in (clang-cl.exe) do set CLANGCL=%%~$PATH:c
if not defined CLANGCL set CLANGCL=%ProgramFiles%\LLVM\bin\clang-cl.exe
if not exist "%CLANGCL%" echo build_win: clang-cl was not found (install LLVM) & exit /b 1
for %%c in (cmake.exe) do set CMAKE=%%~$PATH:c
if not defined CMAKE set CMAKE=%ProgramFiles%\CMake\bin\cmake.exe
if not exist "%CMAKE%" echo build_win: cmake was not found & exit /b 1

cd /d "%~dp0.."
if "%ACTION%"=="build" goto build
set PS3ARG=
if defined PS3RECOMP_DIR set PS3ARG=-DPS3RECOMP_DIR="%PS3RECOMP_DIR:\=/%"
"%CMAKE%" -S . -B %BDIR% -G Ninja %PS3ARG% -DRECOMP_DIR="%CD:\=/%/%RECOMP%" ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_MAKE_PROGRAM="%NINJA:\=/%" ^
  -DCMAKE_C_COMPILER="%CLANGCL:\=/%" ^
  -DCMAKE_CXX_COMPILER="%CLANGCL:\=/%" ^
  -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT:\=/%/scripts/buildsystems/vcpkg.cmake" ^
  -DVCPKG_TARGET_TRIPLET=x64-windows
if %errorlevel% neq 0 exit /b 1
if "%ACTION%"=="configure" exit /b 0
:build
"%CMAKE%" --build %BDIR% -- %2 %3 %4
exit /b %errorlevel%
