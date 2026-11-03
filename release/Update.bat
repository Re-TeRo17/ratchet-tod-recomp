@echo off
rem Update: checks GitHub for a newer version of the port and installs it over this folder.
rem Your settings (tod.ini), saves (gamedata\), shader cache (cache\) and game files stay as they are.
setlocal
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\update.ps1" %*
pause
