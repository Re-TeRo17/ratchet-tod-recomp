@echo off
rem Starts the game. The log goes to log\tod.log (the previous run's to log\tod.previous.log).
setlocal
cd /d "%~dp0"
rem 1.01: only elf\EBOOT.ELF is checked here; the game files can be moved elsewhere with PS3_VFS_ROOT in tod.ini,
rem and tod.exe itself says what is missing.
if not exist "elf\EBOOT.ELF" goto notinstalled
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\play.ps1"
exit /b
:notinstalled
echo The game is not installed yet. Starting the installer...
call "%~dp0Install.bat"
