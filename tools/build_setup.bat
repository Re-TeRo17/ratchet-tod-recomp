@echo off
rem Builds tod_setup.exe (the installer alone: src/install/ + tools/setup/tod_setup.cpp) with MSVC.
rem Needs vcpkg zlib:x64-windows (VCPKG_ROOT, or C:\vcpkg); the exe then needs z.dll beside it.
rem Usage: tools\build_setup.bat [output dir]   (default: build_setup\)
setlocal
rem vcvars64 points VCPKG_ROOT at the copy bundled with Visual Studio: keep ours.
if not defined VCPKG_ROOT set "VCPKG_ROOT=C:\vcpkg"
set "TOD_VCPKG=%VCPKG_ROOT%"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VCVARS="
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -products * -latest -property installationPath`) do set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" echo build_setup: Visual Studio was not found & exit /b 1
call "%VCVARS%" >nul
set "OUT=%~1"
if "%OUT%"=="" set "OUT=%~dp0..\build_setup"
if not exist "%OUT%" mkdir "%OUT%"
set "VCPKG_INC=%TOD_VCPKG%\installed\x64-windows\include"
set "VCPKG_LIB=%TOD_VCPKG%\installed\x64-windows\lib"
cd /d "%~dp0.."
cl /nologo /O2 /std:c++20 /EHsc /MT /utf-8 /DNDEBUG /Isrc /I"%VCPKG_INC%" /Fo"%OUT%\\" /Fe"%OUT%\tod_setup.exe" ^
  tools\setup\tod_setup.cpp src\install\install.cpp src\install\iso.cpp src\install\self.cpp src\install\crypto.cpp ^
  /link /LIBPATH:"%VCPKG_LIB%" z.lib /SUBSYSTEM:CONSOLE
exit /b %errorlevel%
