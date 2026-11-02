@echo off
rem Starts the game. The log goes to log\tod.log (the previous run's to log\tod.previous.log).
setlocal
cd /d "%~dp0"
if not exist "elf\EBOOT.ELF" goto notinstalled
if not exist "game\disc\PS3_GAME\PARAM.SFO" goto notinstalled
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\play.ps1"
exit /b
:notinstalled
echo The game is not installed yet. Starting the installer...
call "%~dp0Install.bat"
